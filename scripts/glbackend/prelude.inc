create
enter
tex rt 4 4 RGBA 1 RENDER_TARGET -
set_rt rt
viewport 0 0 4 4
ortho 0 4 0 4 -100 100
cull NEITHER
clear CLEAR_COLOR 0 0 0 1
vshader vs
uniform float4x4 ViewProj;
struct VI { float4 pos : POSITION; };
VI main(VI v) { VI o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); return o; }
end
pshader ps
uniform float4 color = {1.0, 0.0, 0.0, 1.0};
struct VI { float4 pos : POSITION; };
float4 main(VI v) : TARGET { return color; }
end
vb quad 6 0 0 0
1 1 0
1 3 0
3 1 0
3 1 0
1 3 0
3 3 0
end
load_vb quad
load_vs vs
load_ps ps
