/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
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
 * The Original Code is Copyright (C) 2005 by the Blender Foundation.
 * All rights reserved.
 *
 * Contributor(s): Martin Felke
 *
 * ***** END GPL LICENSE BLOCK *****
 *
 */

/** \file blender/modifiers/intern/MOD_rigidbody.c
 *  \ingroup modifiers
 *
 * Rigid Body modifier
 *
 * Creates rigidbodies from mesh islands
 */

#include "MEM_guardedalloc.h"

#include "BLI_utildefines.h"
#include "BLI_math.h"
#include "BLI_listbase.h"
#include "BLI_kdtree.h"

#include "BKE_cdderivedmesh.h"
#include "BKE_modifier.h"
#include "BKE_rigidbody.h"
#include "BKE_pointcache.h"
#include "BKE_scene.h"
#include "BKE_object.h"
#include "BKE_particle.h"
#include "BKE_group.h"
#include "BKE_depsgraph.h"

#include "bmesh.h"

#include "DNA_object_types.h"
#include "DNA_rigidbody_types.h"
#include "DNA_scene_types.h"
#include "DNA_listBase.h"
#include "DNA_group_types.h"

#include "../../rigidbody/RBI_api.h"


static void initData(ModifierData *md)
{
	RigidBodyModifierData *rmd = (RigidBodyModifierData *) md;
	rmd->visible_mesh = NULL;
	rmd->refresh = TRUE;
	zero_m4(rmd->origmat);
	rmd->breaking_threshold = 10.0f;
	rmd->use_constraints = FALSE;
	rmd->constraint_group = NULL;
	rmd->contact_dist = 0.00001f;
	rmd->group_breaking_threshold = 1.0f;
	rmd->group_contact_dist = 0.0001f;
	rmd->mass_dependent_thresholds = FALSE;
	rmd->auto_merge = FALSE;
	rmd->sel_indexes = NULL;
	rmd->sel_counter = 0;
}

static void copyData(ModifierData *md, ModifierData *target)
{
	RigidBodyModifierData *rmd  = (RigidBodyModifierData *)md;
	RigidBodyModifierData *trmd = (RigidBodyModifierData *)target;
	
	//trmd->meshIslands = rmd->meshIslands;
	trmd->refresh = TRUE;
	trmd->auto_merge = rmd->auto_merge;
	trmd->breaking_threshold = rmd->breaking_threshold;
	trmd->use_constraints = rmd->use_constraints;
	trmd->constraint_group = rmd->constraint_group;
	trmd->contact_dist = rmd->contact_dist;
	trmd->group_breaking_threshold = rmd->group_breaking_threshold;
	trmd->group_contact_dist = rmd->group_contact_dist;
	trmd->mass_dependent_thresholds = rmd->mass_dependent_thresholds;
	/*trmd->sel_indexes = rmd->sel_indexes;
	trmd->sel_counter = rmd->sel_counter;*/
}

static void freeData(ModifierData *md)
{
	RigidBodyModifierData *rmd  = (RigidBodyModifierData *)md;
	MeshIsland *mi;
	RigidBodyShardCon *rbsc;
	int i;

	while (rmd->meshIslands.first) {
		mi = rmd->meshIslands.first;
		BLI_remlink(&rmd->meshIslands, mi);
		if (mi->physics_mesh)
			BM_mesh_free(mi->physics_mesh);
		mi->physics_mesh = NULL;
		if (mi->rigidbody)
			MEM_freeN(mi->rigidbody);
		mi->rigidbody = NULL;
		if (mi->vertco)
			MEM_freeN(mi->vertco);
		mi->vertco = NULL;
		if (mi->vertices)
			MEM_freeN(mi->vertices);
		mi->vertices = NULL;
		MEM_freeN(mi);
		mi = NULL;
	}

	while (rmd->meshConstraints.first) {
		rbsc = rmd->meshConstraints.first;
		BLI_remlink(&rmd->meshConstraints, rbsc);
		MEM_freeN(rbsc);
	}

	rmd->meshIslands.first = NULL;
	rmd->meshIslands.last = NULL;
	rmd->meshConstraints.first = NULL;
	rmd->meshConstraints.last = NULL;

	if (rmd->visible_mesh)
	{
		BM_mesh_free(rmd->visible_mesh);
		rmd->visible_mesh = NULL;
	}

	if (rmd->sel_indexes != NULL) {
		for (i = 0; i < rmd->sel_counter; i++) {
			MEM_freeN(rmd->sel_indexes[i]);
			rmd->sel_indexes[i] = NULL;
		}
		MEM_freeN(rmd->sel_indexes);
		rmd->sel_indexes = NULL;
		rmd->sel_counter = 0;
	}
}

int BM_calc_center_centroid(BMesh *bm, float cent[3], int tagged)
{
	BMFace *f;
	BMIter iter;
	float face_area;
	float total_area = 0.0f;
	float face_cent[3];

	zero_v3(cent);

	/* calculate a weighted average of face centroids */
	BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
		if (BM_elem_flag_test(f, BM_ELEM_TAG) || !tagged) {
			BM_face_calc_center_mean (f, face_cent);
			face_area = BM_face_calc_area(f);

			madd_v3_v3fl(cent, face_cent, face_area);
			total_area += face_area;
		}
	}
	/* otherwise we get NAN for 0 polys */
	if (bm->totface) {
		mul_v3_fl(cent, 1.0f / total_area);
	}

	return (bm->totface != 0);
}

static void mesh_separate_tagged(RigidBodyModifierData* rmd, Object *ob)
{
	BMesh *bm_new;
	BMesh *bm_old = rmd->visible_mesh;
	MeshIsland *mi;
	int vertcount = 0;
	float centroid[3], dummyloc[3], rot[4], *startco;
	BMVert* v, **verts;
	BMIter iter;

	bm_new = BM_mesh_create(&bm_mesh_allocsize_default);
	BM_mesh_elem_toolflags_ensure(bm_new);  /* needed for 'duplicate' bmo */

	CustomData_copy(&bm_old->vdata, &bm_new->vdata, CD_MASK_BMESH, CD_CALLOC, 0);
	CustomData_copy(&bm_old->edata, &bm_new->edata, CD_MASK_BMESH, CD_CALLOC, 0);
	CustomData_copy(&bm_old->ldata, &bm_new->ldata, CD_MASK_BMESH, CD_CALLOC, 0);
	CustomData_copy(&bm_old->pdata, &bm_new->pdata, CD_MASK_BMESH, CD_CALLOC, 0);

	CustomData_bmesh_init_pool(&bm_new->vdata, bm_mesh_allocsize_default.totvert, BM_VERT);
	CustomData_bmesh_init_pool(&bm_new->edata, bm_mesh_allocsize_default.totedge, BM_EDGE);
	CustomData_bmesh_init_pool(&bm_new->ldata, bm_mesh_allocsize_default.totloop, BM_LOOP);
	CustomData_bmesh_init_pool(&bm_new->pdata, bm_mesh_allocsize_default.totface, BM_FACE);

	BMO_op_callf(bm_old, (BMO_FLAG_DEFAULTS & ~BMO_FLAG_RESPECT_HIDE),
	             "duplicate geom=%hvef dest=%p", BM_ELEM_TAG, bm_new);

	BM_calc_center_centroid(bm_new, centroid, FALSE);
	//printf("Centroid: %f %f %f \n", centroid[0], centroid[1], centroid[2]);
	verts = MEM_callocN(sizeof(BMVert*), "mesh_separate_tagged->verts");
	startco = MEM_callocN(sizeof(float), "mesh_separate_tagged->startco");

	//store tagged vertices from old bmesh, important for later manipulation
	//create rigidbody objects with island verts here

	BM_ITER_MESH (v, &iter, bm_new, BM_VERTS_OF_MESH) {
		//eliminate centroid in vertex coords ?
		sub_v3_v3(v->co, centroid);
	}

	BM_ITER_MESH (v, &iter, bm_old, BM_VERTS_OF_MESH) {

		if (BM_elem_flag_test(v, BM_ELEM_TAG)) {
			verts = MEM_reallocN(verts, sizeof(BMVert*) * (vertcount + 1));
			verts[vertcount] = v;
			//sub_v3_v3(v->co, centroid);

			startco = MEM_reallocN(startco, (vertcount+1) * 3 * sizeof(float));
			startco[3 * vertcount] = v->co[0];
			startco[3 * vertcount+1] = v->co[1];
			startco[3 * vertcount+2] = v->co[2];
			vertcount++;
		}
	}

	// add 1 MeshIsland
	mi = MEM_callocN(sizeof(MeshIsland), "meshIsland");
	BLI_addtail(&rmd->meshIslands, mi);

	mi->vertices = verts;
	mi->vertco = startco;
	mi->physics_mesh = bm_new;
	mi->vertex_count = vertcount;
	copy_v3_v3(mi->centroid, centroid);
	mat4_to_loc_quat(dummyloc, rot, ob->obmat);
	copy_v3_v3(mi->rot, rot);
	mi->parent_mod = rmd;
//	mi->cluster_index = -1; //belongs to no cluster

	/* deselect loose data - this used to get deleted,
	 * we could de-select edges and verts only, but this turns out to be less complicated
	 * since de-selecting all skips selection flushing logic */
	//BM_mesh_elem_hflag_disable_all(bm_old, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_SELECT, FALSE);
	BM_mesh_normals_update(bm_new, FALSE);
}

/* flush a hflag to from verts to edges/faces */
static void bm_mesh_hflag_flush_vert(BMesh *bm, const char hflag)
{
	BMEdge *e;
	BMLoop *l_iter;
	BMLoop *l_first;
	BMFace *f;

	BMIter eiter;
	BMIter fiter;

	int ok;

	BM_ITER_MESH (e, &eiter, bm, BM_EDGES_OF_MESH) {
		if (BM_elem_flag_test(e->v1, hflag) &&
		    BM_elem_flag_test(e->v2, hflag))
		{
			BM_elem_flag_enable(e, hflag);
		}
		else {
			BM_elem_flag_disable(e, hflag);
		}
	}
	BM_ITER_MESH (f, &fiter, bm, BM_FACES_OF_MESH) {
		ok = TRUE;
		l_iter = l_first = BM_FACE_FIRST_LOOP(f);
		do {
			if (!BM_elem_flag_test(l_iter->v, hflag)) {
				ok = FALSE;
				break;
			}
		} while ((l_iter = l_iter->next) != l_first);

		BM_elem_flag_set(f, hflag, ok);
	}
}


void mesh_separate_loose(RigidBodyModifierData* rmd, Object* ob)
{
	int i;
	BMEdge *e;
	BMVert *v_seed;
	BMWalker walker;
	int max_iter = 0;
	int tot = 0;
	BMesh* bm_old = rmd->visible_mesh;
	GHash* hash = BLI_ghash_ptr_new("VertIslands");

	max_iter = bm_old->totvert;

	/* Clear all selected vertices */
	BM_mesh_elem_hflag_disable_all(bm_old, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, FALSE);

	/* A "while (true)" loop should work here as each iteration should
	 * select and remove at least one vertex and when all vertices
	 * are selected the loop will break out. But guard against bad
	 * behavior by limiting iterations to the number of vertices in the
	 * original mesh.*/
	for (i = 0; i < max_iter; i++) {
		//int tot = 0;
		BMIter iter;
		BM_ITER_MESH (v_seed, &iter, bm_old, BM_VERTS_OF_MESH) {
			/* Get a seed vertex to start the walk */
			//v_seed = BM_iter_at_index(bm_old, BM_VERTS_OF_MESH, NULL, 0);
			if (!BM_elem_flag_test(v_seed, BM_ELEM_TAG) && !BLI_ghash_haskey(hash, v_seed)) {	//find untagged vertex, better iterate over all verts ?
				//delete old tags HERE, if found untagged vertex, should be on right island now, but... must not be existing yet
				BM_mesh_elem_hflag_disable_all(bm_old, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_SELECT| BM_ELEM_TAG, FALSE);
				break;
			}
		}

		/* No vertices available, can't do anything */
		if (v_seed == NULL){
			break;
		}

		/* Select the seed explicitly, in case it has no edges */
		if (!BM_elem_flag_test(v_seed, BM_ELEM_TAG) && !BLI_ghash_haskey(hash, v_seed)) {
			BLI_ghash_insert(hash, v_seed, v_seed);
			BM_elem_flag_enable(v_seed, BM_ELEM_TAG);
			tot++;
		}

		/* Walk from the single vertex, selecting everything connected
		 * to it */
		BMW_init(&walker, bm_old, BMW_SHELL,
					BMW_MASK_NOP, BMW_MASK_NOP, BMW_MASK_NOP,
					BMW_FLAG_NOP,
					BMW_NIL_LAY);

		e = BMW_begin(&walker, v_seed);
		for (; e; e = BMW_step(&walker)) {
			if (!BM_elem_flag_test(e->v1, BM_ELEM_TAG) && !BLI_ghash_haskey(hash, e->v1)){
				BLI_ghash_insert(hash, e->v1, e->v1);
				BM_elem_flag_enable(e->v1, BM_ELEM_TAG);
				tot++;
			}
			if (!BM_elem_flag_test(e->v2, BM_ELEM_TAG) && !BLI_ghash_haskey(hash, e->v2)) {
				BLI_ghash_insert(hash, e->v2, e->v2);
				BM_elem_flag_enable(e->v2, BM_ELEM_TAG);
				tot++;
			}
		}
		BMW_end(&walker);

		/* Flush the selection to get edge/face selections matching
		 * the vertex selection */
		bm_mesh_hflag_flush_vert(bm_old, BM_ELEM_TAG);


		/* Move selection into a separate object */
		mesh_separate_tagged(rmd, ob);

		if ((tot >= bm_old->totvert) && (BLI_countlist(&rmd->meshIslands) > 1)) {
			// Nothing more to select, work is done
			break;
		}
	}
	BLI_ghash_free(hash, NULL, NULL);
}

void select_inner_faces_of_vert(RigidBodyModifierData* rmd, KDTree* tree, BMVert* vert) {

	BMFace *face;
	BMIter iter;

	BM_ITER_ELEM(face, &iter, vert, BM_FACES_OF_VERT) {
		int n, i = 0;
		float co[3];
		BMFace* f;
		KDTreeNearest *face_near = MEM_mallocN(sizeof(KDTreeNearest) * 2, "kdtreenearest_face");
		BM_face_calc_center_bounds(face, co);
		n = BLI_kdtree_find_n_nearest(tree, 2, co, face->no, face_near);
		//BM_elem_flag_enable(face, BM_ELEM_SELECT);

		for (i = 0; i < n; i++) {
			f = BM_face_at_index(rmd->visible_mesh, face_near[i].index);
			if (BM_elem_flag_test(face, BM_ELEM_TAG) && BM_elem_flag_test(f, BM_ELEM_TAG))
				break;

			//dist = face_near[i].dist;
			if (f->head.index != face->head.index) {
				float res[3], zero[3];
				zero_v3(zero);
				add_v3_v3v3(res, face->no, f->no);
				//printf("Res (%f %f %f) %d %d %f\n", res[0], res[1], res[2], face->head.index, f->head.index, face_near[i].dist);
				if (compare_v3v3(res, zero, 0.000001f))
				{
					BM_elem_flag_enable(f, BM_ELEM_SELECT);
					BM_elem_flag_enable(face, BM_ELEM_SELECT);

					if ((!(BM_elem_flag_test(f, BM_ELEM_TAG)) && (!BM_elem_flag_test(face, BM_ELEM_TAG))) || 1) {
						rmd->sel_indexes = MEM_reallocN(rmd->sel_indexes, sizeof(int*) * (rmd->sel_counter+1));
						rmd->sel_indexes[rmd->sel_counter] = MEM_callocN(sizeof(int)*2, "sel_index_pair");
						rmd->sel_indexes[rmd->sel_counter][0] = f->head.index;
						rmd->sel_indexes[rmd->sel_counter][1] = face->head.index;
						rmd->sel_counter++;
					}

					BM_elem_flag_enable(f, BM_ELEM_TAG);
				}
			}
			else
			{
				//BM_elem_flag_disable(f, BM_ELEM_SELECT);
				BM_elem_flag_enable(f, BM_ELEM_TAG);
			}

		}
		MEM_freeN(face_near);
		face_near = NULL;
	}
}

static void connect_constraints(RigidBodyModifierData* rmd,  Object* ob, MeshIsland **meshIslands, int count, BMesh **combined_mesh, KDTree **combined_tree) {

	MeshIsland *mi;
	BMOpSlot *slot;
	int i, j, v, sel_counter;
	KDTreeNearest *n = MEM_mallocN(sizeof(KDTreeNearest)*count, "kdtreenearest");
	KDTreeNearest *n2 = MEM_mallocN(sizeof(KDTreeNearest)*count, "kdtreenearest2");

	KDTree* face_tree;
	BMFace *fa;
	BMIter bmi;

	face_tree = BLI_kdtree_new(rmd->visible_mesh->totface);
	BM_ITER_MESH(fa, &bmi, rmd->visible_mesh, BM_FACES_OF_MESH)
	{
		float co[3];
		BM_face_calc_center_bounds(fa, co);
		BLI_kdtree_insert(face_tree, fa->head.index, co, fa->no);
	}

	BLI_kdtree_balance(face_tree);

	BLI_kdtree_find_n_nearest(*combined_tree, count, meshIslands[0]->centroid, NULL, n2);

	for (j = 0; j < count; j++) {
		int same = FALSE;
		mi = meshIslands[(n2+j)->index];

		for (v = 0; v < mi->vertex_count; v++) {
			BM_elem_flag_enable(BM_vert_at_index(*combined_mesh, mi->combined_index_map[v]), BM_ELEM_TAG);
		}

		BLI_kdtree_find_n_nearest(*combined_tree, count, mi->centroid, NULL, n);

		for (i = 0; i < count; i++) {
			MeshIsland *mi2 = meshIslands[(n+i)->index];
			//printf("Nearest: %d %d %f %f %f\n",m, (n+i)->index, (n+i)->co[0], (n+i)->co[2], (n+i)->co[2]);

			if ((mi != mi2) && (mi2 != NULL)) {
				BMOperator op;
				int shared = 0, island_vert_key_index = 0, island_vert_map_index = 0;
				int* island_verts_key = MEM_mallocN(sizeof(int), "island_verts_key");
				int* island_verts_map = MEM_mallocN(sizeof(int), "island_verts_map");

				//check whether we are in the same object or not
				float thresh, dist;
				int equal = mi->parent_mod == mi2->parent_mod;
				//equal = equal && (mi->parent_mod == rmd);

				thresh = equal ? rmd->breaking_threshold : rmd->group_breaking_threshold;
				dist = equal ? rmd->contact_dist : rmd->group_contact_dist;

				//select "our" vertices
				for (v = 0; v < mi2->vertex_count; v++) {
					BM_elem_flag_enable(BM_vert_at_index(*combined_mesh, mi2->combined_index_map[v]), BM_ELEM_TAG);
				}

				//do we share atleast 1 vertex in selection

				BMO_op_initf(*combined_mesh, &op, (BMO_FLAG_DEFAULTS & ~BMO_FLAG_RESPECT_HIDE), "find_doubles verts=%hv dist=%f", BM_ELEM_TAG, dist);
				BMO_op_exec(*combined_mesh, &op);
				slot = BMO_slot_get(op.slots_out, "targetmap.out");

				if (slot->data.ghash) {
					GHashIterator it;
					int ind1 = 0, ind2 = 0;
					GHASH_ITER(it, slot->data.ghash) {
						BMVert *vert_key, *vert_map;
						BMOElemMapping * mapping = BLI_ghashIterator_getValue(&it);
						vert_key = BLI_ghashIterator_getKey(&it);
						vert_map = mapping[1].element;

						if (vert_key == vert_map)
						{
							printf("EQUAL! D'OH\n");
						}

						for (v = 0; v < mi->vertex_count; v++) {
							if ((vert_key == BM_vert_at_index(*combined_mesh, mi->combined_index_map[v]))) {
								island_verts_key = MEM_reallocN(island_verts_key, sizeof(int) * (island_vert_key_index+1));
								island_verts_key[island_vert_key_index] = mi->combined_index_map[v];
								island_vert_key_index++;
								ind1++;
								break;
							}

							if ((vert_map == BM_vert_at_index(*combined_mesh, mi->combined_index_map[v]))) {
								island_verts_map = MEM_reallocN(island_verts_map, sizeof(int) * (island_vert_map_index+1));
								island_verts_map[island_vert_map_index] = mi->combined_index_map[v];
								island_vert_map_index++;
								break;
							}
						}

						for (v = 0; v < mi2->vertex_count; v++) {
							if ((vert_key == BM_vert_at_index(*combined_mesh, mi2->combined_index_map[v]))) {
								island_verts_key = MEM_reallocN(island_verts_key, sizeof(int) * (island_vert_key_index+1));
								island_verts_key[island_vert_key_index] = mi2->combined_index_map[v];
								island_vert_key_index++;
								ind2++;
								break;
							}

							if ((vert_map == BM_vert_at_index(*combined_mesh, mi2->combined_index_map[v]))) {
								island_verts_map = MEM_reallocN(island_verts_map, sizeof(int) * (island_vert_map_index+1));
								island_verts_map[island_vert_map_index] = mi2->combined_index_map[v];
								island_vert_map_index++;
								break;
							}
						}
					}

					// verts are in different objects, ok, only if we have different modifiers as parent
					same = ((equal && ((ind1 == 0) || (ind2 == 0))) || (!equal));
				}

				//if ((slot->data.ghash) && (slot->data.ghash->nentries > 2))
				//	printf("%d %d\n", island_vert_key_index, island_vert_map_index);

				if (rmd->auto_merge && ((island_vert_key_index > 0) || (island_vert_map_index > 0))) {
					BMVert **vert_arr, **vert_arr2;
					int s;

					vert_arr = MEM_mallocN(sizeof(BMVert*) * island_vert_key_index, "vert_arr");
					vert_arr2 = MEM_mallocN(sizeof(BMVert*) * island_vert_map_index, "vert_arr2");

					for (s = 0; s < island_vert_key_index; s++) {
						vert_arr[s] = BM_vert_at_index(rmd->visible_mesh, island_verts_key[s]);
					}

					for (s = 0; s < island_vert_map_index; s++) {
						vert_arr2[s] = BM_vert_at_index(rmd->visible_mesh, island_verts_map[s]);
					}

					//select_inner_faces_of_vert(rmd, face_tree, vert_arr[0]);
					//select_inner_faces_of_vert(rmd, face_tree, vert_arr2[0]);
					if (island_vert_key_index > 0) {
						for (s = 0; s < island_vert_key_index; s++)
						{
							select_inner_faces_of_vert(rmd, face_tree, vert_arr[s]);
						}
					}

					if (island_vert_map_index > 0) {
						for (s = 0; s < island_vert_map_index; s++)
						{
							select_inner_faces_of_vert(rmd, face_tree, vert_arr2[s]);
						}
					}

					MEM_freeN(vert_arr);
					MEM_freeN(vert_arr2);
					vert_arr = NULL;
					vert_arr2 = NULL;
				}

				MEM_freeN(island_verts_map);
				MEM_freeN(island_verts_key);
				island_verts_map = NULL;
				island_verts_key = NULL;
				island_vert_key_index = 0;
				island_vert_map_index = 0;

				if (slot->data.ghash) {
					shared = slot->data.ghash->nentries;
				}
				else
				{
					shared = 0;
				}

				if ((rmd->auto_merge) && (shared > 0)) {
					float co[3], co2[3];
					copy_v3_v3(co, mi->centroid);
					copy_v3_v3(co2, mi2->centroid);
					printf("MeshIsland: %d %d (%f, %f, %f) | (%f, %f, %f) - %d \n",
						   j, (n+i)->index, co[0], co[1], co[2], co2[0], co2[1], co2[2], shared);
				}

				BMO_op_finish(*combined_mesh, &op);
				slot = NULL;

				//deselect vertices
				for (v = 0; v < mi2->vertex_count; v++) {
					BM_elem_flag_disable(BM_vert_at_index(*combined_mesh, mi2->combined_index_map[v]), BM_ELEM_TAG);
				}

				if (shared > 0) {
					// shared vertices (atleast one face ?), so connect...
					// if all verts either in same object or not !
					int con_found = FALSE;
					RigidBodyShardCon *con, *rbsc;

					for (con = rmd->meshConstraints.first; con; con = con->next) {
						if (((con->mi1 == mi) && (con->mi2 == mi2)) ||
							((con->mi1 == mi2 && (con->mi2 == mi)))) {
							con_found = TRUE;
							break;
						}
					}
					if ((!con_found) && same){
						if (rmd->use_constraints) {
							if (((rmd->constraint_group != NULL) &&
								(!object_in_group(ob, rmd->constraint_group))) ||
								(rmd->constraint_group == NULL)) {

								rbsc = BKE_rigidbody_create_shard_constraint(rmd->modifier.scene, RBC_TYPE_FIXED);
								rbsc->mi1 = mi;
								rbsc->mi2 = mi2;
								rbsc->breaking_threshold = thresh;
								BLI_addtail(&rmd->meshConstraints, rbsc);
							}
						}
					}
				}
			}
		}

		for (v = 0; v < mi->vertex_count; v++) {
			BM_elem_flag_disable(BM_vert_at_index(*combined_mesh, mi->combined_index_map[v]), BM_ELEM_TAG);
		}
	}

	sel_counter = 0;
	BM_ITER_MESH(fa, &bmi, rmd->visible_mesh, BM_FACES_OF_MESH)
	{
		if (!BM_elem_flag_test(fa, BM_ELEM_TAG) && rmd->auto_merge)
		{
			printf("Face %d NOT visited\n", fa->head.index);
		}
		if (BM_elem_flag_test(fa, BM_ELEM_SELECT)) {
			sel_counter++;
		}
	}

	printf(" %d faces selected\n", sel_counter);

	MEM_freeN(n);
	MEM_freeN(n2);
	BLI_kdtree_free(face_tree);
	face_tree = NULL;
}

static int create_combined_neighborhood(RigidBodyModifierData *rmd, MeshIsland ***mesh_islands, BMesh **combined_mesh, KDTree **combined_tree)
{
	ModifierData* md;
	RigidBodyModifierData* rmd2;
	GroupObject* go;
	MeshIsland* mi;
	int v, i = 0, islands = 0, vert_counter = 0;

	//create a combined mesh over all part bmeshes, and a combined kdtree to find "outer" constraints as well
	//handle single object here
	*combined_mesh = BM_mesh_create(&bm_mesh_allocsize_default);
	BM_mesh_elem_toolflags_ensure(*combined_mesh);

	islands = BLI_countlist(&rmd->meshIslands);
	*mesh_islands = MEM_reallocN(*mesh_islands, islands*sizeof(MeshIsland*));
	for (mi = rmd->meshIslands.first; mi; mi = mi->next) {
		mi->combined_index_map = MEM_mallocN(mi->vertex_count*sizeof(int), "combined_index_map");
		for (v = 0; v < mi->vertex_count; v++) {
			float co[3];
			copy_v3_v3(co, mi->vertices[v]->co);
			BM_vert_create(*combined_mesh, co, NULL, 0);
			mi->combined_index_map[v] = vert_counter;
			vert_counter++;
		}
		(*mesh_islands)[i] = mi;
		i++;
	}

	//handle a group of objects to be taken account into as well
	if (rmd->constraint_group != NULL) {
		for (go = rmd->constraint_group->gobject.first; go; go = go->next) {
			for (md = go->ob->modifiers.first; md; md = md->next) {
				if (md->type == eModifierType_RigidBody) {
					rmd2 = (RigidBodyModifierData*)md;
					rmd2->constraint_group = rmd->constraint_group;
					islands += BLI_countlist(&rmd2->meshIslands);
					*mesh_islands = MEM_reallocN(*mesh_islands, islands*sizeof(MeshIsland*));
					for (mi = rmd2->meshIslands.first; mi; mi = mi->next) {
						mi->combined_index_map = MEM_mallocN(mi->vertex_count*sizeof(int), "combined_index_map");
						for (v = 0; v < mi->vertex_count; v++) {
							float co[3];
							copy_v3_v3(co, mi->vertices[v]->co);
							BM_vert_create(*combined_mesh, co, NULL, 0);
							mi->combined_index_map[v] = vert_counter;
							vert_counter++;
						}
						(*mesh_islands)[i] = mi;
						i++;
					}
					//rmd2->refresh = TRUE;
				}
				//break;
			}
		}
	}

	*combined_tree = BLI_kdtree_new(islands);
	for (i = 0; i < islands; i++) {
		BLI_kdtree_insert(*combined_tree, i, (*mesh_islands)[i]->centroid, NULL);
	}

	BLI_kdtree_balance(*combined_tree);

	return islands;
}

static void create_constraints(RigidBodyModifierData *rmd, Object* ob) {
	KDTree* combined_tree = NULL;
	BMesh* combined_mesh = NULL;
	MeshIsland** mesh_islands = MEM_mallocN(sizeof(MeshIsland*), "mesh_islands");
	int count, i = 0;

	count = create_combined_neighborhood(rmd, &mesh_islands, &combined_mesh, &combined_tree);

	if ((combined_mesh != NULL) && (combined_tree != NULL))
		connect_constraints(rmd, ob, mesh_islands, count, &combined_mesh, &combined_tree);

	if (combined_tree != NULL) {
		BLI_kdtree_free(combined_tree);
		combined_tree = NULL;
	}

	if (combined_mesh != NULL) {
		BM_mesh_free(combined_mesh);
		combined_mesh = NULL;
	}

	for (i = 0; i < count; i++) {
		MEM_freeN(mesh_islands[i]->combined_index_map);
	}

	MEM_freeN(mesh_islands);
}

BMFace* findClosestFace(KDTree* tree, BMesh* bm, BMFace* f)
{
	int index;
	float co[3];
	BMFace* f2;

	BM_face_calc_center_bounds(f, co);
	index = BLI_kdtree_find_nearest(tree, co, NULL, NULL);
	f2 = BM_face_at_index(bm, index);
	return f2;
}

BMFace* closest_available_face(RigidBodyModifierData* rmd, KDTree* tree, BMesh* bm,  BMFace* f, BMFace* f2)
{
	float co[3], co2[3];

	if (!f) return findClosestFace(tree, bm, f2);

	BM_face_calc_center_bounds(f, co);
	BM_face_calc_center_bounds(f2, co2);

	if (!compare_v3v3(co, co2, rmd->group_contact_dist)) {
		return findClosestFace(tree, bm, f2);
	}

	return NULL;
}

void check_face_draw_by_constraint(RigidBodyModifierData* rmd, BMesh* merge_copy) {

	int sel = rmd->sel_counter;
	int i = 0;
	BMFace *face, *face2;

	for (i = 0; i < sel; i++)
	{
		face = BM_face_at_index(merge_copy, rmd->sel_indexes[i][0]);
		face2 = BM_face_at_index(merge_copy, rmd->sel_indexes[i][1]);
		BM_elem_flag_enable(face, BM_ELEM_TAG);
		BM_elem_flag_enable(face2, BM_ELEM_TAG);
		if (BLI_countlist(&rmd->meshConstraints) > 0) {
			RigidBodyShardCon* con = BLI_findlink(&rmd->meshConstraints,i);
			if (con && con->physics_constraint) {
				if (RB_constraint_is_enabled(con->physics_constraint)) {
					BM_elem_flag_enable(face, BM_ELEM_SELECT);
					BM_elem_flag_enable(face2, BM_ELEM_SELECT);
				}
				else
				{
					BM_elem_flag_disable(face, BM_ELEM_SELECT);
					BM_elem_flag_disable(face2, BM_ELEM_SELECT);
				}
			}
			else
			{
				BM_elem_flag_disable(face, BM_ELEM_SELECT);
				BM_elem_flag_disable(face2, BM_ELEM_SELECT);
			}
		}
		else
		{
			BM_elem_flag_disable(face, BM_ELEM_SELECT);
			BM_elem_flag_disable(face2, BM_ELEM_SELECT);
		}
	}
}

void check_face_draw_by_proximity(RigidBodyModifierData* rmd, BMesh* merge_copy) {

	int sel = rmd->sel_counter;
	int i = 0;
	KDTree* tree;
	BMFace* face;
	BMIter iter;

	//merge vertices
	BMO_op_callf(merge_copy, BMO_FLAG_DEFAULTS, "automerge verts=%av dist=%f", BM_VERTS_OF_MESH, 0.00001f);

	tree = BLI_kdtree_new(rmd->visible_mesh->totface);

	//build tree of left selected faces
	BM_ITER_MESH(face, &iter, merge_copy, BM_FACES_OF_MESH)
	{
		if (BM_elem_flag_test(face, BM_ELEM_TAG))
		{
			float co[3];
			BM_face_calc_center_bounds(face, co);
			BLI_kdtree_insert(tree, face->head.index, co, NULL);
		}
	}

	BLI_kdtree_balance(tree);

	//delete invisible inner faces, check which faces have been merged away, delete those who still there ?
	for (i = 0; i < sel; i++) {

		BMFace *f, *f2, *f3, *f4;
		int index = rmd->sel_indexes[i][0];
		int index2 = rmd->sel_indexes[i][1];

		f = BM_face_at_index(merge_copy, index);
		f2 = BM_face_at_index(rmd->visible_mesh, index);
		f3 = closest_available_face(rmd, tree, merge_copy, f, f2);

		f = BM_face_at_index(merge_copy, index2);
		f2 = BM_face_at_index(rmd->visible_mesh, index2);
		f4 = closest_available_face(rmd, tree, merge_copy, f, f2);

		if (f3 != NULL) {
			BM_elem_flag_enable(f3, BM_ELEM_SELECT);
		}

		if (f4 != NULL) {
			BM_elem_flag_enable(f4, BM_ELEM_SELECT);
		}
	}

	BLI_kdtree_free(tree);
	tree = NULL;
}

static DerivedMesh *applyModifier(ModifierData *md, Object *ob,
								  DerivedMesh *dm,
								  ModifierApplyFlag UNUSED(flag))
{

	RigidBodyModifierData *rmd = (RigidBodyModifierData *) md;

	/*if (rmd->sel_indexes == NULL) {
		rmd->sel_indexes = MEM_callocN(sizeof(int*), "sel_indexes");
		rmd->sel_counter = 0;
	}*/

	if (rmd->refresh)
	{
		freeData(md);
		copy_m4_m4(rmd->origmat, ob->obmat);
		rmd->visible_mesh = DM_to_bmesh(dm);
		mesh_separate_loose(rmd, ob);

		if ((rmd->use_constraints) || (rmd->auto_merge)) {
			if (((rmd->constraint_group != NULL) && (!object_in_group(ob, rmd->constraint_group))) ||
					(rmd->constraint_group == NULL) || (rmd->auto_merge)) {
				create_constraints(rmd, ob); //check for actually creating the constraints inside
			}
		}

		rmd->refresh = FALSE;
	}

	if (rmd->visible_mesh != NULL) {

		DerivedMesh *dm_final;
		if (rmd->auto_merge) {
			BMesh* merge_copy = BM_mesh_copy(rmd->visible_mesh);

			check_face_draw_by_constraint(rmd, merge_copy);
			if (rmd->group_contact_dist > 0)
			{
				check_face_draw_by_proximity(rmd, merge_copy);
			}

			BMO_op_callf(merge_copy, (BMO_FLAG_DEFAULTS & ~BMO_FLAG_RESPECT_HIDE),
						"delete geom=%hvef context=%i", BM_ELEM_SELECT, DEL_FACES);

			//final merge to close gaps
			BMO_op_callf(merge_copy, BMO_FLAG_DEFAULTS, "automerge verts=%hv dist=%f", BM_ELEM_SELECT, rmd->group_contact_dist);

			dm_final = CDDM_from_bmesh(merge_copy, TRUE);
			BM_mesh_free(merge_copy);
		}
		else
		{
			dm_final = CDDM_from_bmesh(rmd->visible_mesh, TRUE);
		}
		return dm_final;
	}
	else {
		return dm;
	}
}

static int dependsOnTime(ModifierData *UNUSED(md))
{
	return 1;
}

static void foreachIDLink(ModifierData *md, Object *ob,
						  IDWalkFunc walk, void *userData)
{
	RigidBodyModifierData *rmd = (RigidBodyModifierData *) md;

	walk(userData, ob, (ID **)&rmd->constraint_group);
}

ModifierTypeInfo modifierType_RigidBody = {
	/* name */              "RigidBody",
	/* structName */        "RigidBodyModifierData",
	/* structSize */        sizeof(RigidBodyModifierData),
	/* type */              eModifierTypeType_Constructive,
	/* flags */             eModifierTypeFlag_AcceptsMesh |
							eModifierTypeFlag_UsesPointCache |
							eModifierTypeFlag_Single,
	
	/* copyData */          copyData,
	/* deformVerts */       NULL,
	/* deformMatrices */    NULL,
	/* deformVertsEM */     NULL,
	/* deformMatricesEM */  NULL,
	/* applyModifier */     applyModifier,
	/* applyModifierEM */   NULL,
	/* initData */          initData,
	/* requiredDataMask */  NULL,
	/* freeData */          freeData,
	/* isDisabled */        NULL,
	/* updateDepgraph */    NULL,
	/* dependsOnTime */     dependsOnTime,
	/* dependsOnNormals */	NULL,
	/* foreachObjectLink */ NULL,
	/* foreachIDLink */     foreachIDLink,
	/* foreachTexLink */    NULL
};
