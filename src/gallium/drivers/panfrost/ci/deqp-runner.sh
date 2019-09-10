#!/bin/sh

DEQP_OPTIONS="--deqp-surface-width=256 --deqp-surface-height=256"
DEQP_OPTIONS="$DEQP_OPTIONS --deqp-visibility=hidden"
DEQP_OPTIONS="$DEQP_OPTIONS --deqp-log-images=disable"
DEQP_OPTIONS="$DEQP_OPTIONS --deqp-watchdog=enable"
DEQP_OPTIONS="$DEQP_OPTIONS --deqp-crashhandler=enable"
DEQP_OPTIONS="$DEQP_OPTIONS --deqp-surface-type=pbuffer"

export LIBGL_DRIVERS_PATH=/mesa/lib/dri/
export LD_LIBRARY_PATH=/mesa/lib/
export MESA_GLES_VERSION_OVERRIDE=3.0

DEVFREQ_GOVERNOR=`echo /sys/devices/platform/*.gpu/devfreq/devfreq0/governor`
echo performance > $DEVFREQ_GOVERNOR

cd /deqp/modules/gles2

# Generate test case list file
./deqp-gles2 $DEQP_OPTIONS --deqp-runmode=stdout-caselist | grep "TEST: dEQP-GLES2" | cut -d ' ' -f 2 > /tmp/case-list.txt

# Disable for now tests that are very slow, either by just using lots of CPU or by crashing
FLIP_FLOPS="
    dEQP-GLES2.performance
    dEQP-GLES2.stress
    dEQP-GLES2.functional.fbo.render.depth.
    dEQP-GLES2.functional.flush_finish.
    "

# These pass or fail seemingly at random
FLIP_FLOPS="$FLIP_FLOPS
    dEQP-GLES2.performance
    dEQP-GLES2.stress
    dEQP-GLES2.functional.fbo.render.depth.
    dEQP-GLES2.functional.flush_finish.
    dEQP-GLES2.functional.clipping.triangle_vertex.clip_three.clip_neg_x_neg_z_and_pos_x_pos_z_and_neg_x_neg_y_pos_z
    dEQP-GLES2.functional.clipping.triangle_vertex.clip_three.clip_pos_y_pos_z_and_neg_x_neg_y_pos_z_and_pos_x_pos_y_neg_z
    dEQP-GLES2.functional.fbo.render.color.blend_rbo_rgb5_a1
    dEQP-GLES2.functional.fbo.render.color.blend_rbo_rgb5_a1_depth_component16
    dEQP-GLES2.functional.fbo.render.color.blend_rbo_rgba4
    dEQP-GLES2.functional.fbo.render.color.blend_rbo_rgba4_depth_component16
    dEQP-GLES2.functional.fbo.render.color.blend_npot_rbo_rgb5_a1
    dEQP-GLES2.functional.fbo.render.color.blend_npot_rbo_rgb5_a1_depth_component16
    dEQP-GLES2.functional.fbo.render.color.blend_npot_rbo_rgba4
    dEQP-GLES2.functional.fbo.render.color.blend_npot_rbo_rgba4_depth_component16
    dEQP-GLES2.functional.fbo.render.color_clear.rbo_rgb5_a1
    dEQP-GLES2.functional.fbo.render.color_clear.rbo_rgb5_a1_depth_component16
    dEQP-GLES2.functional.fbo.render.color_clear.rbo_rgb5_a1_stencil_index8
    dEQP-GLES2.functional.fbo.render.color_clear.rbo_rgba4_depth_component16
    dEQP-GLES2.functional.fbo.render.color_clear.rbo_rgba4_stencil_index8
    dEQP-GLES2.functional.fbo.render.recreate_depthbuffer.
    dEQP-GLES2.functional.fbo.render.recreate_stencilbuffer.
    dEQP-GLES2.functional.fbo.render.shared_colorbuffer_clear.rbo_rgb5_a1
    dEQP-GLES2.functional.fbo.render.shared_colorbuffer_clear.rbo_rgba4
    dEQP-GLES2.functional.fbo.render.shared_colorbuffer_clear.tex2d_rgb
    dEQP-GLES2.functional.fbo.render.shared_colorbuffer_clear.tex2d_rgba
    dEQP-GLES2.functional.fbo.render.shared_colorbuffer.rbo_rgb5_a1
    dEQP-GLES2.functional.fbo.render.shared_colorbuffer.rbo_rgba4
    dEQP-GLES2.functional.fbo.render.shared_depthbuffer.rbo_rgb5_a1_depth_component16
    dEQP-GLES2.functional.fbo.render.shared_depthbuffer.rbo_rgba4_depth_component16
    dEQP-GLES2.functional.fbo.render.stencil_clear.rbo_rgb5_a1_stencil_index8
    dEQP-GLES2.functional.fbo.render.stencil.npot_rbo_rgb5_a1_stencil_index8
    dEQP-GLES2.functional.fbo.render.stencil.npot_rbo_rgba4_stencil_index8
    dEQP-GLES2.functional.fbo.render.stencil.rbo_rgb5_a1_stencil_index8
    dEQP-GLES2.functional.fbo.render.stencil.rbo_rgba4_stencil_index8
    dEQP-GLES2.functional.lifetime.attach.deleted_input.renderbuffer_framebuffer
    dEQP-GLES2.functional.lifetime.attach.deleted_output.renderbuffer_framebuffer
    dEQP-GLES2.functional.polygon_offset.fixed16_factor_0_slope
    dEQP-GLES2.functional.polygon_offset.fixed16_factor_1_slope
    dEQP-GLES2.functional.shaders.invariance.highp.loop_4
    dEQP-GLES2.functional.shaders.matrix.mul.dynamic_highp_mat4_vec4_vertex
    dEQP-GLES2.functional.shaders.matrix.mul.dynamic_highp_vec4_mat4_fragment
    dEQP-GLES2.functional.shaders.operator.common_functions.smoothstep.mediump_vec3_vertex
    dEQP-GLES2.functional.shaders.random.all_features.fragment.12
    dEQP-GLES2.functional.shaders.random.all_features.fragment.37
    dEQP-GLES2.functional.texture.units.2_units.mixed.1
    dEQP-GLES2.functional.texture.units.2_units.mixed.3
    dEQP-GLES2.functional.texture.units.2_units.only_2d.2
    dEQP-GLES2.functional.texture.units.4_units.mixed.5
    dEQP-GLES2.functional.texture.units.4_units.only_2d.0
    dEQP-GLES2.functional.texture.units.8_units.only_cube.2
    dEQP-GLES2.functional.texture.units.all_units.mixed.6
    dEQP-GLES2.functional.texture.units.all_units.only_cube.4
    dEQP-GLES2.functional.texture.units.all_units.only_cube.7
    dEQP-GLES2.functional.texture.units.all_units.only_cube.8
    "

for test in $FLIP_FLOPS; do sed -i "/$test/d" /tmp/case-list.txt; done

/deqp/deqp-volt --cts-build-dir=/deqp \
                --threads=8 \
                --test-names-file=/tmp/case-list.txt \
                --results-file=/tmp/results.txt \
                --no-passed-results \
                --regression-file=/deqp/expected-failures.txt \
                --no-rerun-tests \
                --print-regression \
                --no-print-fail \
                --no-print-quality \
                --no-colour-term \
                 $DEQP_OPTIONS

if [ $? -ne 0 ]; then
    echo "Regressions detected"
    echo "deqp: fail"
else
    echo "No regressions detected"
    echo "deqp: pass"
fi
