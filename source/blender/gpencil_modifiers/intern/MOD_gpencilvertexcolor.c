/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software  Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2017, Blender Foundation
 * This is a new part of Blender
 */

/** \file
 * \ingroup modifiers
 */

#include <stdio.h>

#include "BLI_utildefines.h"

#include "BLI_math.h"
#include "BLI_listbase.h"

#include "DNA_meshdata_types.h"
#include "DNA_scene_types.h"
#include "DNA_object_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_gpencil_modifier_types.h"
#include "DNA_modifier_types.h"

#include "BKE_action.h"
#include "BKE_colorband.h"
#include "BKE_colortools.h"
#include "BKE_deform.h"
#include "BKE_gpencil.h"
#include "BKE_gpencil_modifier.h"
#include "BKE_modifier.h"
#include "BKE_library_query.h"
#include "BKE_scene.h"
#include "BKE_main.h"
#include "BKE_layer.h"

#include "MEM_guardedalloc.h"

#include "MOD_gpencil_util.h"
#include "MOD_gpencil_modifiertypes.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_build.h"
#include "DEG_depsgraph_query.h"

static void initData(GpencilModifierData *md)
{
  VertexcolorGpencilModifierData *gpmd = (VertexcolorGpencilModifierData *)md;
  gpmd->pass_index = 0;
  gpmd->layername[0] = '\0';
  gpmd->materialname[0] = '\0';
  gpmd->vgname[0] = '\0';
  gpmd->object = NULL;
  gpmd->radius = 1.0f;
  gpmd->factor = 1.0f;

  /* Add default color ramp. */
  gpmd->colorband = BKE_colorband_add(false);
  if (gpmd->colorband) {
    BKE_colorband_init(gpmd->colorband, true);
    CBData *ramp = gpmd->colorband->data;
    ramp[0].r = ramp[0].g = ramp[0].b = ramp[0].a = 1.0f;
    ramp[0].pos = 0.0f;
    ramp[1].r = ramp[1].g = ramp[1].b = 0.0f;
    ramp[1].a = 1.0f;
    ramp[1].pos = 1.0f;

    gpmd->colorband->tot = 2;
  }
}

static void copyData(const GpencilModifierData *md, GpencilModifierData *target)
{
  VertexcolorGpencilModifierData *gmd = (VertexcolorGpencilModifierData *)md;
  VertexcolorGpencilModifierData *tgmd = (VertexcolorGpencilModifierData *)target;

  MEM_SAFE_FREE(tgmd->colorband);

  BKE_gpencil_modifier_copyData_generic(md, target);
  if (gmd->colorband) {
    tgmd->colorband = MEM_dupallocN(gmd->colorband);
  }
}

static void gpencil_parent_location(const Depsgraph *depsgraph,
                                    Object *ob,
                                    bGPDlayer *gpl,
                                    float diff_mat[4][4])
{
  Object *ob_eval = depsgraph != NULL ? DEG_get_evaluated_object(depsgraph, ob) : ob;
  Object *obparent = gpl->parent;
  Object *obparent_eval = depsgraph != NULL ? DEG_get_evaluated_object(depsgraph, obparent) :
                                              obparent;

  /* if not layer parented, try with object parented */
  if (obparent_eval == NULL) {
    if (ob_eval != NULL) {
      copy_m4_m4(diff_mat, ob_eval->obmat);
      return;
    }
    unit_m4(diff_mat);
    return;
  }
  else {
    if ((gpl->partype == PAROBJECT) || (gpl->partype == PARSKEL)) {
      mul_m4_m4m4(diff_mat, obparent_eval->obmat, gpl->inverse);
      add_v3_v3(diff_mat[3], ob_eval->obmat[3]);
      return;
    }
    else if (gpl->partype == PARBONE) {
      bPoseChannel *pchan = BKE_pose_channel_find_name(obparent_eval->pose, gpl->parsubstr);
      if (pchan) {
        float tmp_mat[4][4];
        mul_m4_m4m4(tmp_mat, obparent_eval->obmat, pchan->pose_mat);
        mul_m4_m4m4(diff_mat, tmp_mat, gpl->inverse);
        add_v3_v3(diff_mat[3], ob_eval->obmat[3]);
      }
      else {
        /* if bone not found use object (armature) */
        mul_m4_m4m4(diff_mat, obparent_eval->obmat, gpl->inverse);
        add_v3_v3(diff_mat[3], ob_eval->obmat[3]);
      }
      return;
    }
    else {
      unit_m4(diff_mat); /* not defined type */
    }
  }
}

/* Check if a point is inside a ellipsoid. */
static bool gpencil_check_inside_ellipsoide(float co[3],
                                            float radius[3],
                                            float obmat[4][4],
                                            float inv_mat[4][4])
{
  float fpt[3];

  /* Translate to Ellipsoid space. */
  sub_v3_v3v3(fpt, co, obmat[3]);

  /* Rotate point to ellipsoid rotation. */
  mul_mat3_m4_v3(inv_mat, fpt);

  /* Standard equation of an ellipsoid. */
  float r = ((fpt[0] / radius[0]) * (fpt[0] / radius[0])) +
            ((fpt[1] / radius[1]) * (fpt[1] / radius[1])) +
            ((fpt[2] / radius[2]) * (fpt[2] / radius[2]));

  if (r < 1.0f) {
    return true;
  }
  return false;
}

/* deform stroke */
static void deformStroke(GpencilModifierData *md,
                         Depsgraph *depsgraph,
                         Object *ob,
                         bGPDlayer *gpl,
                         bGPDframe *UNUSED(gpf),
                         bGPDstroke *gps)
{
  VertexcolorGpencilModifierData *mmd = (VertexcolorGpencilModifierData *)md;
  if (!mmd->object) {
    return;
  }

  const int def_nr = defgroup_name_index(ob, mmd->vgname);

  if (!is_stroke_affected_by_modifier(ob,
                                      mmd->layername,
                                      mmd->materialname,
                                      mmd->pass_index,
                                      mmd->layer_pass,
                                      1,
                                      gpl,
                                      gps,
                                      mmd->flag & GP_HOOK_INVERT_LAYER,
                                      mmd->flag & GP_HOOK_INVERT_PASS,
                                      mmd->flag & GP_HOOK_INVERT_LAYERPASS,
                                      mmd->flag & GP_HOOK_INVERT_MATERIAL)) {
    return;
  }

  float target_scale = mat4_to_scale(mmd->object->obmat);
  float radius_sqr = (mmd->radius * mmd->radius) * target_scale;
  float coba_res[4];
  float mat[4][4];

  gpencil_parent_location(depsgraph, ob, gpl, mat);

  /* Radius and matrix for Ellipsoid. */
  float radius[3];
  float inv_mat[4][4];
  mul_v3_v3fl(radius, mmd->object->scale, mmd->radius);
  /* Clamp to avoid division by zero. */
  CLAMP3_MIN(radius, 0.0001f);

  invert_m4_m4(inv_mat, mmd->object->obmat);

  /* loop points and apply deform */
  bool doit = false;
  for (int i = 0; i < gps->totpoints; i++) {
    bGPDspoint *pt = &gps->points[i];
    MDeformVert *dvert = gps->dvert != NULL ? &gps->dvert[i] : NULL;

    /* Calc world position of point. */
    float pt_loc[3];
    mul_v3_m4v3(pt_loc, mat, &pt->x);
    float dist_sqr = len_squared_v3v3(pt_loc, mmd->object->loc);

    if (!gpencil_check_inside_ellipsoide(pt_loc, radius, mmd->object->obmat, inv_mat)) {
      continue;
    }

    if (!doit) {
      /* Apply to fill. */
      if (mmd->mode != GPPAINT_MODE_STROKE) {
        BKE_colorband_evaluate(mmd->colorband, 1.0f, coba_res);
        interp_v3_v3v3(gps->vert_color_fill, gps->vert_color_fill, coba_res, mmd->factor);
        gps->vert_color_fill[3] = mmd->factor;
        /* If no stroke, cancel loop. */
        if (mmd->mode != GPPAINT_MODE_BOTH) {
          break;
        }
      }

      doit = true;
    }

    /* Verify vertex group. */
    if (mmd->mode != GPPAINT_MODE_FILL) {
      const float weight = get_modifier_point_weight(
          dvert, (mmd->flag & GP_HOOK_INVERT_VGROUP) != 0, def_nr);
      if (weight < 0.0f) {
        continue;
      }
      /* Calc the factor using the distance and get mix color. */
      float mix_factor = dist_sqr / radius_sqr;
      BKE_colorband_evaluate(mmd->colorband, mix_factor, coba_res);

      interp_v3_v3v3(pt->vert_color, pt->vert_color, coba_res, mmd->factor * weight);
      pt->vert_color[3] = mmd->factor;
      /* Apply Decay. */
      if (mmd->flag & GP_VERTEXCOL_DECAY_COLOR) {
        pt->vert_color[3] *= (1.0f - mix_factor);
      }
    }
  }
}

/* FIXME: Ideally we be doing this on a copy of the main depsgraph
 * (i.e. one where we don't have to worry about restoring state)
 */
static void bakeModifier(Main *bmain, Depsgraph *depsgraph, GpencilModifierData *md, Object *ob)
{
  VertexcolorGpencilModifierData *mmd = (VertexcolorGpencilModifierData *)md;
  Scene *scene = DEG_get_evaluated_scene(depsgraph);
  bGPdata *gpd = ob->data;
  int oldframe = (int)DEG_get_ctime(depsgraph);

  if (mmd->object == NULL) {
    return;
  }

  LISTBASE_FOREACH (bGPDlayer *, gpl, &gpd->layers) {
    LISTBASE_FOREACH (bGPDframe *, gpf, &gpl->frames) {
      /* apply effects on this frame
       * NOTE: this assumes that we don't want animation on non-keyframed frames
       */
      CFRA = gpf->framenum;
      BKE_scene_graph_update_for_newframe(depsgraph, bmain);

      /* compute effects on this frame */
      LISTBASE_FOREACH (bGPDstroke *, gps, &gpf->strokes) {
        deformStroke(md, depsgraph, ob, gpl, gpf, gps);
      }
    }
  }

  /* return frame state and DB to original state */
  CFRA = oldframe;
  BKE_scene_graph_update_for_newframe(depsgraph, bmain);
}

static void freeData(GpencilModifierData *md)
{
  VertexcolorGpencilModifierData *mmd = (VertexcolorGpencilModifierData *)md;
  if (mmd->colorband) {
    MEM_freeN(mmd->colorband);
    mmd->colorband = NULL;
  }
}

static bool isDisabled(GpencilModifierData *md, int UNUSED(userRenderParams))
{
  VertexcolorGpencilModifierData *mmd = (VertexcolorGpencilModifierData *)md;

  return !mmd->object;
}

static void updateDepsgraph(GpencilModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  VertexcolorGpencilModifierData *lmd = (VertexcolorGpencilModifierData *)md;
  if (lmd->object != NULL) {
    DEG_add_object_relation(ctx->node, lmd->object, DEG_OB_COMP_GEOMETRY, "Vertexcolor Modifier");
    DEG_add_object_relation(ctx->node, lmd->object, DEG_OB_COMP_TRANSFORM, "Vertexcolor Modifier");
  }
  DEG_add_object_relation(ctx->node, ctx->object, DEG_OB_COMP_TRANSFORM, "Vertexcolor Modifier");
}

static void foreachObjectLink(GpencilModifierData *md,
                              Object *ob,
                              ObjectWalkFunc walk,
                              void *userData)
{
  VertexcolorGpencilModifierData *mmd = (VertexcolorGpencilModifierData *)md;

  walk(userData, ob, &mmd->object, IDWALK_CB_NOP);
}

GpencilModifierTypeInfo modifierType_Gpencil_Vertexcolor = {
    /* name */ "Vertexcolor",
    /* structName */ "VertexcolorGpencilModifierData",
    /* structSize */ sizeof(VertexcolorGpencilModifierData),
    /* type */ eGpencilModifierTypeType_Gpencil,
    /* flags */ eGpencilModifierTypeFlag_SupportsEditmode,

    /* copyData */ copyData,

    /* deformStroke */ deformStroke,
    /* generateStrokes */ NULL,
    /* bakeModifier */ bakeModifier,
    /* remapTime */ NULL,

    /* initData */ initData,
    /* freeData */ freeData,
    /* isDisabled */ isDisabled,
    /* updateDepsgraph */ updateDepsgraph,
    /* dependsOnTime */ NULL,
    /* foreachObjectLink */ foreachObjectLink,
    /* foreachIDLink */ NULL,
    /* foreachTexLink */ NULL,
};