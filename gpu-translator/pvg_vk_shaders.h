/*
 * pvg_vk_shaders.h - Embedded SPIR-V for the PVG (Paravirtualized Graphics)
 *                    Vulkan present pipeline.
 *
 * Two shaders that draw a fullscreen textured quad to blit the guest's
 * rendered frame into the host swapchain ("present"):
 *
 *   - SimpleVertex          (vertex stage)
 *   - SimpleTextureFragment (fragment stage)
 *
 * GLSL source (Vulkan 4.50, --target-env vulkan1.0):
 *
 *   // SimpleVertex
 *   #version 450
 *   layout(push_constant) uniform PushConstants { mat4 mvp; } pc;
 *   layout(location = 0) in  vec2 pos;
 *   layout(location = 1) in  vec2 tex;
 *   layout(location = 0) out vec2 uv;
 *   void main() {
 *       gl_Position = pc.mvp * vec4(pos, 0.0, 1.0);
 *       uv = tex;
 *   }
 *
 *   // SimpleTextureFragment
 *   #version 450
 *   layout(set = 0, binding = 0) uniform sampler2D src;
 *   layout(location = 0) in  vec2 uv;
 *   layout(location = 0) out vec4 outColor;
 *   void main() {
 *       outColor = texture(src, uv);
 *   }
 *
 * Conventions / wiring notes:
 *   - Vulkan GLSL.  Y-flip is NOT done in the shader; the present code must
 *     flip Y at the viewport (negative-height VkViewport, or y = height,
 *     height = -height) so the host image is not upside down.
 *   - MVP: 64-byte push-constant block at offset 0, stage VERTEX.
 *   - src: combined image sampler, descriptor set 0, binding 0, stage FRAGMENT.
 *   - Vertex inputs: location 0 = pos (vec2), location 1 = tex (vec2).
 *   - Each module exposes a single OpEntryPoint whose name is given by the
 *     PVG_VK_*_ENTRY macros below; pass it as VkPipelineShaderStageCreateInfo.pName.
 *
 * Generated with glslangValidator (Glslang 11:16.3.0, SPIR-V 1.0):
 *   glslangValidator -V --target-env vulkan1.0 \
 *       --source-entrypoint main -e SimpleVertex          simple.vert -o simple.vert.spv
 *   glslangValidator -V --target-env vulkan1.0 \
 *       --source-entrypoint main -e SimpleTextureFragment simple.frag -o simple.frag.spv
 *
 * DO NOT EDIT the arrays by hand; regenerate from the GLSL above.
 */

#ifndef HW_DISPLAY_PVG_VK_SHADERS_H
#define HW_DISPLAY_PVG_VK_SHADERS_H

#include <stdint.h>

/* Entry-point names baked into the SPIR-V (use as VkPipelineShaderStageCreateInfo.pName). */
#define PVG_VK_SIMPLE_VERTEX_ENTRY           "SimpleVertex"
#define PVG_VK_SIMPLE_TEXTURE_FRAGMENT_ENTRY "SimpleTextureFragment"

/* ------------------------------------------------------------------ *
 * SimpleVertex - vertex stage
 * ------------------------------------------------------------------ */
static const uint32_t pvg_vk_simple_vertex_spv[] = {
    0x07230203u, 0x00010000u, 0x0008000bu, 0x00000027u, 0x00000000u,
    0x00020011u, 0x00000001u, 0x0006000bu, 0x00000001u, 0x4c534c47u,
    0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u,
    0x00000001u, 0x000b000fu, 0x00000000u, 0x00000004u, 0x706d6953u,
    0x6556656cu, 0x78657472u, 0x00000000u, 0x0000000du, 0x00000019u,
    0x00000024u, 0x00000025u, 0x00030003u, 0x00000002u, 0x000001c2u,
    0x00060005u, 0x00000004u, 0x706d6953u, 0x6556656cu, 0x78657472u,
    0x00000000u, 0x00060005u, 0x0000000bu, 0x505f6c67u, 0x65567265u,
    0x78657472u, 0x00000000u, 0x00060006u, 0x0000000bu, 0x00000000u,
    0x505f6c67u, 0x7469736fu, 0x006e6f69u, 0x00070006u, 0x0000000bu,
    0x00000001u, 0x505f6c67u, 0x746e696fu, 0x657a6953u, 0x00000000u,
    0x00070006u, 0x0000000bu, 0x00000002u, 0x435f6c67u, 0x4470696cu,
    0x61747369u, 0x0065636eu, 0x00070006u, 0x0000000bu, 0x00000003u,
    0x435f6c67u, 0x446c6c75u, 0x61747369u, 0x0065636eu, 0x00030005u,
    0x0000000du, 0x00000000u, 0x00060005u, 0x00000011u, 0x68737550u,
    0x736e6f43u, 0x746e6174u, 0x00000073u, 0x00040006u, 0x00000011u,
    0x00000000u, 0x0070766du, 0x00030005u, 0x00000013u, 0x00006370u,
    0x00030005u, 0x00000019u, 0x00736f70u, 0x00030005u, 0x00000024u,
    0x00007675u, 0x00030005u, 0x00000025u, 0x00786574u, 0x00030047u,
    0x0000000bu, 0x00000002u, 0x00050048u, 0x0000000bu, 0x00000000u,
    0x0000000bu, 0x00000000u, 0x00050048u, 0x0000000bu, 0x00000001u,
    0x0000000bu, 0x00000001u, 0x00050048u, 0x0000000bu, 0x00000002u,
    0x0000000bu, 0x00000003u, 0x00050048u, 0x0000000bu, 0x00000003u,
    0x0000000bu, 0x00000004u, 0x00030047u, 0x00000011u, 0x00000002u,
    0x00040048u, 0x00000011u, 0x00000000u, 0x00000005u, 0x00050048u,
    0x00000011u, 0x00000000u, 0x00000007u, 0x00000010u, 0x00050048u,
    0x00000011u, 0x00000000u, 0x00000023u, 0x00000000u, 0x00040047u,
    0x00000019u, 0x0000001eu, 0x00000000u, 0x00040047u, 0x00000024u,
    0x0000001eu, 0x00000000u, 0x00040047u, 0x00000025u, 0x0000001eu,
    0x00000001u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u,
    0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u,
    0x00000007u, 0x00000006u, 0x00000004u, 0x00040015u, 0x00000008u,
    0x00000020u, 0x00000000u, 0x0004002bu, 0x00000008u, 0x00000009u,
    0x00000001u, 0x0004001cu, 0x0000000au, 0x00000006u, 0x00000009u,
    0x0006001eu, 0x0000000bu, 0x00000007u, 0x00000006u, 0x0000000au,
    0x0000000au, 0x00040020u, 0x0000000cu, 0x00000003u, 0x0000000bu,
    0x0004003bu, 0x0000000cu, 0x0000000du, 0x00000003u, 0x00040015u,
    0x0000000eu, 0x00000020u, 0x00000001u, 0x0004002bu, 0x0000000eu,
    0x0000000fu, 0x00000000u, 0x00040018u, 0x00000010u, 0x00000007u,
    0x00000004u, 0x0003001eu, 0x00000011u, 0x00000010u, 0x00040020u,
    0x00000012u, 0x00000009u, 0x00000011u, 0x0004003bu, 0x00000012u,
    0x00000013u, 0x00000009u, 0x00040020u, 0x00000014u, 0x00000009u,
    0x00000010u, 0x00040017u, 0x00000017u, 0x00000006u, 0x00000002u,
    0x00040020u, 0x00000018u, 0x00000001u, 0x00000017u, 0x0004003bu,
    0x00000018u, 0x00000019u, 0x00000001u, 0x0004002bu, 0x00000006u,
    0x0000001bu, 0x00000000u, 0x0004002bu, 0x00000006u, 0x0000001cu,
    0x3f800000u, 0x00040020u, 0x00000021u, 0x00000003u, 0x00000007u,
    0x00040020u, 0x00000023u, 0x00000003u, 0x00000017u, 0x0004003bu,
    0x00000023u, 0x00000024u, 0x00000003u, 0x0004003bu, 0x00000018u,
    0x00000025u, 0x00000001u, 0x00050036u, 0x00000002u, 0x00000004u,
    0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u, 0x00050041u,
    0x00000014u, 0x00000015u, 0x00000013u, 0x0000000fu, 0x0004003du,
    0x00000010u, 0x00000016u, 0x00000015u, 0x0004003du, 0x00000017u,
    0x0000001au, 0x00000019u, 0x00050051u, 0x00000006u, 0x0000001du,
    0x0000001au, 0x00000000u, 0x00050051u, 0x00000006u, 0x0000001eu,
    0x0000001au, 0x00000001u, 0x00070050u, 0x00000007u, 0x0000001fu,
    0x0000001du, 0x0000001eu, 0x0000001bu, 0x0000001cu, 0x00050091u,
    0x00000007u, 0x00000020u, 0x00000016u, 0x0000001fu, 0x00050041u,
    0x00000021u, 0x00000022u, 0x0000000du, 0x0000000fu, 0x0003003eu,
    0x00000022u, 0x00000020u, 0x0004003du, 0x00000017u, 0x00000026u,
    0x00000025u, 0x0003003eu, 0x00000024u, 0x00000026u, 0x000100fdu,
    0x00010038u,
};
/* ------------------------------------------------------------------ *
 * SimpleTextureFragment - fragment stage
 * ------------------------------------------------------------------ */
static const uint32_t pvg_vk_simple_texture_fragment_spv[] = {
    0x07230203u, 0x00010000u, 0x0008000bu, 0x00000014u, 0x00000000u,
    0x00020011u, 0x00000001u, 0x0006000bu, 0x00000001u, 0x4c534c47u,
    0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u,
    0x00000001u, 0x000b000fu, 0x00000004u, 0x00000004u, 0x706d6953u,
    0x6554656cu, 0x72757478u, 0x61724665u, 0x6e656d67u, 0x00000074u,
    0x00000009u, 0x00000011u, 0x00030010u, 0x00000004u, 0x00000007u,
    0x00030003u, 0x00000002u, 0x000001c2u, 0x00080005u, 0x00000004u,
    0x706d6953u, 0x6554656cu, 0x72757478u, 0x61724665u, 0x6e656d67u,
    0x00000074u, 0x00050005u, 0x00000009u, 0x4374756fu, 0x726f6c6fu,
    0x00000000u, 0x00030005u, 0x0000000du, 0x00637273u, 0x00030005u,
    0x00000011u, 0x00007675u, 0x00040047u, 0x00000009u, 0x0000001eu,
    0x00000000u, 0x00040047u, 0x0000000du, 0x00000021u, 0x00000000u,
    0x00040047u, 0x0000000du, 0x00000022u, 0x00000000u, 0x00040047u,
    0x00000011u, 0x0000001eu, 0x00000000u, 0x00020013u, 0x00000002u,
    0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u,
    0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u, 0x00000004u,
    0x00040020u, 0x00000008u, 0x00000003u, 0x00000007u, 0x0004003bu,
    0x00000008u, 0x00000009u, 0x00000003u, 0x00090019u, 0x0000000au,
    0x00000006u, 0x00000001u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000001u, 0x00000000u, 0x0003001bu, 0x0000000bu, 0x0000000au,
    0x00040020u, 0x0000000cu, 0x00000000u, 0x0000000bu, 0x0004003bu,
    0x0000000cu, 0x0000000du, 0x00000000u, 0x00040017u, 0x0000000fu,
    0x00000006u, 0x00000002u, 0x00040020u, 0x00000010u, 0x00000001u,
    0x0000000fu, 0x0004003bu, 0x00000010u, 0x00000011u, 0x00000001u,
    0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u,
    0x000200f8u, 0x00000005u, 0x0004003du, 0x0000000bu, 0x0000000eu,
    0x0000000du, 0x0004003du, 0x0000000fu, 0x00000012u, 0x00000011u,
    0x00050057u, 0x00000007u, 0x00000013u, 0x0000000eu, 0x00000012u,
    0x0003003eu, 0x00000009u, 0x00000013u, 0x000100fdu, 0x00010038u,
};
#endif /* HW_DISPLAY_PVG_VK_SHADERS_H */
