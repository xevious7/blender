uniform vec2 invrendertargetdim;

//texture coordinates for framebuffer read
varying vec4 uvcoordsvar;

/* color texture coordinates, offset by a small amount */
varying vec2 color_uv1;
varying vec2 color_uv2;

//very simple shader for gull screen FX, just pass values on

void vert_generic()
{
	uvcoordsvar = gl_MultiTexCoord0;
	gl_Position = gl_Vertex;
}

void vert_dof_first_pass()
{
	uvcoordsvar = gl_MultiTexCoord0;

	/* we offset the texture coordinates by 1.5 pixel, then we reuse that to sample the surrounding pixels */
	color_uv1 = gl_MultiTexCoord0.xy + vec2(-1.5, -1.5) * invrendertargetdim;
	color_uv1 = gl_MultiTexCoord0.xy + vec2(0.5, -1.5) * invrendertargetdim;

	gl_Position = gl_Vertex;
}

void main()
{
#ifdef FIRST_PASS
	vert_dof_first_pass();
#elif defined(SECOND_PASS)
	vert_generic();
#elif defined(THIRD_PASS)
#elif defined(FOURTH_PASS)
#else
	vert_generic();
#endif
}

