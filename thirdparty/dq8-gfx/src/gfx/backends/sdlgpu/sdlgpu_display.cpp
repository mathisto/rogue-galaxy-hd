#include "gfx/backends/sdlgpu/sdlgpu_device.h"
#include "gfx/backends/sdlgpu/sdlgpu_window.h"

#include "gs_display.frag.spv.h"
#include "gs_reinterpret.vert.spv.h"
#include "present_easu.frag.spv.h"
#include "present_downscale.frag.spv.h"
#include "present_rcas.frag.spv.h"
#include "present_sides.frag.spv.h"
#ifdef DQ8_GFX_HAS_MSL
#include "gs_display.frag.msl.h"
#include "gs_reinterpret.vert.msl.h"
#endif

#include <algorithm>
#include <cmath>

namespace dq8::gfx {

double sdlGpuWideAspect(uint32_t windowWidth, uint32_t windowHeight) {
    const double window = double(std::max(windowWidth, 1u)) / double(std::max(windowHeight, 1u));
    return std::clamp(window, 4.0 / 3.0, 32.0 / 9.0);
}

double sdlGpuDisplayAspect(const SdlGpuDisplayOptions &options, uint32_t sourceWidth,
                           uint32_t sourceHeight, uint32_t windowWidth, uint32_t windowHeight,
                           int gameWidescreen) {
    switch (options.aspect) {
    case SdlGpuAspect::Auto:
        return gameWidescreen == 1 ? sdlGpuWideAspect(windowWidth, windowHeight) : 4.0 / 3.0;
    case SdlGpuAspect::Standard:
        return 4.0 / 3.0;
    case SdlGpuAspect::Wide:
        return 16.0 / 9.0;
    case SdlGpuAspect::Native:
        return double(std::max(sourceWidth, 1u)) / double(std::max(sourceHeight, 1u));
    case SdlGpuAspect::Stretch:
        break;
    }
    return double(std::max(windowWidth, 1u)) / double(std::max(windowHeight, 1u));
}

SdlGpuRect sdlGpuDisplayRect(const SdlGpuDisplayOptions &options, uint32_t sourceWidth,
                             uint32_t sourceHeight, uint32_t windowWidth, uint32_t windowHeight,
                             int gameWidescreen) {
    if (sourceWidth == 0u || sourceHeight == 0u || windowWidth == 0u || windowHeight == 0u)
        return {};
    const double aspect = sdlGpuDisplayAspect(options, sourceWidth, sourceHeight, windowWidth,
                                              windowHeight, gameWidescreen);
    double width = windowWidth;
    double height = width / aspect;
    if (height > windowHeight) {
        height = windowHeight;
        width = height * aspect;
    }
    if (options.integerScale && options.aspect != SdlGpuAspect::Stretch) {
        // The largest whole multiple of the source height whose width at this
        // aspect still fits; square pixels then come out exact on both axes.
        for (uint32_t factor = static_cast<uint32_t>(windowHeight / sourceHeight); factor >= 1u; --factor) {
            const double scaledHeight = double(sourceHeight) * factor;
            const double scaledWidth = options.aspect == SdlGpuAspect::Native
                                           ? double(sourceWidth) * factor
                                           : scaledHeight * aspect;
            if (scaledWidth <= windowWidth + 0.5) {
                width = scaledWidth;
                height = scaledHeight;
                break;
            }
        }
    }
    SdlGpuRect rect;
    rect.width = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(width)), 1u, windowWidth);
    rect.height = std::clamp<uint32_t>(static_cast<uint32_t>(std::lround(height)), 1u, windowHeight);
    rect.x = (windowWidth - rect.width) / 2u;
    rect.y = (windowHeight - rect.height) / 2u;
    return rect;
}

bool SdlGpuDevice::composeDisplay(SDL_GPUTexture *circuit1, SDL_GPUTexture *circuit2,
                                  SDL_GPUTexture *destination, uint32_t width, uint32_t height,
                                  const GsDisplayUniforms &uniforms, SDL_GPUFence *&completed, std::string &error) {
    if (!m_displayPipeline) {
        SDL_GPUShaderCreateInfo vertexInfo{};
        vertexInfo.code = reinterpret_cast<const uint8_t *>(kGsReinterpretVertSpirv);
        vertexInfo.code_size = sizeof(kGsReinterpretVertSpirv);
        vertexInfo.entrypoint = "main";
        vertexInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
        vertexInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
        SDL_GPUShaderCreateInfo fragmentInfo{};
        fragmentInfo.code = reinterpret_cast<const uint8_t *>(kGsDisplayFragSpirv);
        fragmentInfo.code_size = sizeof(kGsDisplayFragSpirv);
        fragmentInfo.entrypoint = "main";
        fragmentInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
        fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        fragmentInfo.num_samplers = 2u;
        fragmentInfo.num_uniform_buffers = 1u;
#ifdef DQ8_GFX_HAS_MSL
        if ((SDL_GetGPUShaderFormats(m_device) & SDL_GPU_SHADERFORMAT_MSL) != 0) {
            vertexInfo.code = reinterpret_cast<const uint8_t *>(kGsReinterpretVertSpirvMsl);
            vertexInfo.code_size = sizeof(kGsReinterpretVertSpirvMsl);
            vertexInfo.entrypoint = "main0";
            vertexInfo.format = SDL_GPU_SHADERFORMAT_MSL;
            fragmentInfo.code = reinterpret_cast<const uint8_t *>(kGsDisplayFragSpirvMsl);
            fragmentInfo.code_size = sizeof(kGsDisplayFragSpirvMsl);
            fragmentInfo.entrypoint = "main0";
            fragmentInfo.format = SDL_GPU_SHADERFORMAT_MSL;
        }
#endif
        SDL_GPUShader *vertex = SDL_CreateGPUShader(m_device, &vertexInfo);
        if (!vertex) {
            error = std::string("SDL_CreateGPUShader(display vertex): ") + SDL_GetError();
            return false;
        }
        SDL_GPUShader *fragment = SDL_CreateGPUShader(m_device, &fragmentInfo);
        if (!fragment) {
            error = std::string("SDL_CreateGPUShader(display fragment): ") + SDL_GetError();
            SDL_ReleaseGPUShader(m_device, vertex);
            return false;
        }
        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = vertex;
        info.fragment_shader = fragment;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.target_info.color_target_descriptions = &colorTarget;
        info.target_info.num_color_targets = 1u;
        m_displayPipeline = SDL_CreateGPUGraphicsPipeline(m_device, &info);
        SDL_ReleaseGPUShader(m_device, fragment);
        SDL_ReleaseGPUShader(m_device, vertex);
        if (!m_displayPipeline) {
            error = std::string("SDL_CreateGPUGraphicsPipeline(display): ") + SDL_GetError();
            return false;
        }
    }

    SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(m_device);
    if (!commands) {
        error = std::string("SDL_AcquireGPUCommandBuffer(display): ") + SDL_GetError();
        return false;
    }
    SDL_GPUColorTargetInfo target{};
    target.texture = destination;
    target.load_op = SDL_GPU_LOADOP_DONT_CARE;
    target.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &target, 1u, nullptr);
    if (!pass) {
        error = std::string("SDL_BeginGPURenderPass(display): ") + SDL_GetError();
        SDL_CancelGPUCommandBuffer(commands);
        return false;
    }
    SDL_BindGPUGraphicsPipeline(pass, m_displayPipeline);
    const SDL_GPUViewport viewport{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
    SDL_SetGPUViewport(pass, &viewport);
    const SDL_GPUTextureSamplerBinding bindings[] = {{circuit1, m_sampler}, {circuit2, m_sampler}};
    SDL_BindGPUFragmentSamplers(pass, 0u, bindings, 2u);
    SDL_PushGPUFragmentUniformData(commands, 0u, &uniforms, sizeof(uniforms));
    SDL_DrawGPUPrimitives(pass, 3u, 1u, 0u, 0u);
    SDL_EndGPURenderPass(pass);
    completed = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (!completed) {
        error = std::string("SDL_SubmitGPUCommandBufferAndAcquireFence(display): ") + SDL_GetError();
        return false;
    }
    return true;
}

bool SdlGpuDevice::drawSides(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *target, SDL_GPUTextureFormat format,
                             uint32_t width, uint32_t height, SDL_GPUTexture *source, uint32_t sourceWidth,
                             uint32_t sourceHeight, bool blur, float brightness, std::string &error) {
    // SPIR-V only: elsewhere the bars stay black.
    if ((SDL_GetGPUShaderFormats(m_device) & SDL_GPU_SHADERFORMAT_SPIRV) == 0) {
        error = "no SPIR-V shaders on this device";
        return false;
    }
    if (!m_linearSampler) {
        SDL_GPUSamplerCreateInfo samplerInfo{};
        samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
        samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
        samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        m_linearSampler = SDL_CreateGPUSampler(m_device, &samplerInfo);
        if (!m_linearSampler) {
            error = std::string("SDL_CreateGPUSampler(sides): ") + SDL_GetError();
            return false;
        }
    }
    if (m_sidesPipeline && m_sidesFormat != format) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_sidesPipeline);
        m_sidesPipeline = nullptr;
    }
    if (!m_sidesPipeline) {
        SDL_GPUShaderCreateInfo vertexInfo{};
        vertexInfo.code = reinterpret_cast<const uint8_t *>(kGsReinterpretVertSpirv);
        vertexInfo.code_size = sizeof(kGsReinterpretVertSpirv);
        vertexInfo.entrypoint = "main";
        vertexInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
        vertexInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
        SDL_GPUShaderCreateInfo fragmentInfo{};
        fragmentInfo.code = reinterpret_cast<const uint8_t *>(kPresentSidesFragSpirv);
        fragmentInfo.code_size = sizeof(kPresentSidesFragSpirv);
        fragmentInfo.entrypoint = "main";
        fragmentInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
        fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        fragmentInfo.num_samplers = 1u;
        fragmentInfo.num_uniform_buffers = 1u;
        SDL_GPUShader *vertex = SDL_CreateGPUShader(m_device, &vertexInfo);
        if (!vertex) {
            error = std::string("SDL_CreateGPUShader(sides vertex): ") + SDL_GetError();
            return false;
        }
        SDL_GPUShader *fragment = SDL_CreateGPUShader(m_device, &fragmentInfo);
        if (!fragment) {
            error = std::string("SDL_CreateGPUShader(sides fragment): ") + SDL_GetError();
            SDL_ReleaseGPUShader(m_device, vertex);
            return false;
        }
        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = format;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = vertex;
        info.fragment_shader = fragment;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.target_info.color_target_descriptions = &colorTarget;
        info.target_info.num_color_targets = 1u;
        m_sidesPipeline = SDL_CreateGPUGraphicsPipeline(m_device, &info);
        SDL_ReleaseGPUShader(m_device, fragment);
        SDL_ReleaseGPUShader(m_device, vertex);
        if (!m_sidesPipeline) {
            error = std::string("SDL_CreateGPUGraphicsPipeline(sides): ") + SDL_GetError();
            return false;
        }
        m_sidesFormat = format;
    }

    SDL_GPUColorTargetInfo targetInfo{};
    targetInfo.texture = target;
    targetInfo.load_op = SDL_GPU_LOADOP_DONT_CARE;
    targetInfo.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &targetInfo, 1u, nullptr);
    if (!pass) {
        error = std::string("SDL_BeginGPURenderPass(sides): ") + SDL_GetError();
        return false;
    }
    SDL_BindGPUGraphicsPipeline(pass, m_sidesPipeline);
    const SDL_GPUViewport viewport{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
    SDL_SetGPUViewport(pass, &viewport);
    const SDL_GPUTextureSamplerBinding binding{source, m_linearSampler};
    SDL_BindGPUFragmentSamplers(pass, 0u, &binding, 1u);
    const float uniforms[8] = {float(width), float(height), brightness, blur ? 1.0f : 0.0f,
                               float(sourceWidth), float(sourceHeight), 0.0f, 0.0f};
    SDL_PushGPUFragmentUniformData(commands, 0u, uniforms, sizeof(uniforms));
    SDL_DrawGPUPrimitives(pass, 3u, 1u, 0u, 0u);
    SDL_EndGPURenderPass(pass);
    return true;
}

namespace {

// A full-screen pass's pipeline: gs_reinterpret.vert's covering triangle,
// a fragment shader with one sampler and one uniform buffer, no blending.
template <size_t N>
SDL_GPUGraphicsPipeline *createPresentPass(SDL_GPUDevice *device, const uint32_t (&fragmentCode)[N],
                                           SDL_GPUTextureFormat format, const char *name, std::string &error) {
    SDL_GPUShaderCreateInfo vertexInfo{};
    vertexInfo.code = reinterpret_cast<const uint8_t *>(kGsReinterpretVertSpirv);
    vertexInfo.code_size = sizeof(kGsReinterpretVertSpirv);
    vertexInfo.entrypoint = "main";
    vertexInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
    vertexInfo.stage = SDL_GPU_SHADERSTAGE_VERTEX;
    SDL_GPUShaderCreateInfo fragmentInfo{};
    fragmentInfo.code = reinterpret_cast<const uint8_t *>(fragmentCode);
    fragmentInfo.code_size = sizeof(fragmentCode);
    fragmentInfo.entrypoint = "main";
    fragmentInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
    fragmentInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    fragmentInfo.num_samplers = 1u;
    fragmentInfo.num_uniform_buffers = 1u;
    SDL_GPUShader *vertex = SDL_CreateGPUShader(device, &vertexInfo);
    SDL_GPUShader *fragment = vertex ? SDL_CreateGPUShader(device, &fragmentInfo) : nullptr;
    SDL_GPUGraphicsPipeline *pipeline = nullptr;
    if (fragment) {
        SDL_GPUColorTargetDescription colorTarget{};
        colorTarget.format = format;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = vertex;
        info.fragment_shader = fragment;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.target_info.color_target_descriptions = &colorTarget;
        info.target_info.num_color_targets = 1u;
        pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
    }
    if (!pipeline)
        error = std::string(fragment ? "SDL_CreateGPUGraphicsPipeline(" : "SDL_CreateGPUShader(") + name +
                "): " + SDL_GetError();
    if (fragment)
        SDL_ReleaseGPUShader(device, fragment);
    if (vertex)
        SDL_ReleaseGPUShader(device, vertex);
    return pipeline;
}

// One full-screen pass of `pipeline` into rect of target, reading source.
bool runPresentPass(SDL_GPUCommandBuffer *commands, SDL_GPUGraphicsPipeline *pipeline, SDL_GPUTexture *target,
                    const uint32_t (&rect)[4], bool clear, SDL_GPUTexture *source, SDL_GPUSampler *sampler,
                    const float (&uniforms)[8], const char *name, std::string &error) {
    SDL_GPUColorTargetInfo targetInfo{};
    targetInfo.texture = target;
    targetInfo.load_op = clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    targetInfo.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
    targetInfo.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(commands, &targetInfo, 1u, nullptr);
    if (!pass) {
        error = std::string("SDL_BeginGPURenderPass(") + name + "): " + SDL_GetError();
        return false;
    }
    SDL_BindGPUGraphicsPipeline(pass, pipeline);
    const SDL_GPUViewport viewport{float(rect[0]), float(rect[1]), float(rect[2]), float(rect[3]), 0.0f, 1.0f};
    SDL_SetGPUViewport(pass, &viewport);
    const SDL_Rect scissor{int(rect[0]), int(rect[1]), int(rect[2]), int(rect[3])};
    SDL_SetGPUScissor(pass, &scissor);
    const SDL_GPUTextureSamplerBinding binding{source, sampler};
    SDL_BindGPUFragmentSamplers(pass, 0u, &binding, 1u);
    SDL_PushGPUFragmentUniformData(commands, 0u, uniforms, sizeof(uniforms));
    SDL_DrawGPUPrimitives(pass, 3u, 1u, 0u, 0u);
    SDL_EndGPURenderPass(pass);
    return true;
}

} // namespace

bool SdlGpuDevice::drawEasu(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *target, uint32_t width,
                            uint32_t height, SDL_GPUTexture *source, const uint32_t (&region)[4],
                            std::string &error) {
    if ((SDL_GetGPUShaderFormats(m_device) & SDL_GPU_SHADERFORMAT_SPIRV) == 0) {
        error = "no SPIR-V shaders on this device";
        return false;
    }
    if (!m_easuPipeline)
        m_easuPipeline =
            createPresentPass(m_device, kPresentEasuFragSpirv, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, "easu", error);
    if (!m_easuPipeline)
        return false;
    const uint32_t whole[4] = {0u, 0u, width, height};
    const float uniforms[8] = {float(region[0]), float(region[1]), float(region[2]), float(region[3]),
                               float(width),     float(height),    0.0f,             0.0f};
    return runPresentPass(commands, m_easuPipeline, target, whole, false, source, m_sampler, uniforms, "easu", error);
}

bool SdlGpuDevice::drawDownscale(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *target, uint32_t width,
                                 uint32_t height, SDL_GPUTexture *source, const uint32_t (&region)[4], bool down,
                                 std::string &error) {
    if ((SDL_GetGPUShaderFormats(m_device) & SDL_GPU_SHADERFORMAT_SPIRV) == 0) {
        error = "no SPIR-V shaders on this device";
        return false;
    }
    if (!m_downscalePipeline)
        m_downscalePipeline = createPresentPass(m_device, kPresentDownscaleFragSpirv,
                                                SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, "downscale", error);
    if (!m_downscalePipeline)
        return false;
    const uint32_t whole[4] = {0u, 0u, width, height};
    const float uniforms[8] = {float(region[0]), float(region[1]), float(region[2]), float(region[3]),
                               float(width),     float(height),    down ? 1.0f : 0.0f, 0.0f};
    return runPresentPass(commands, m_downscalePipeline, target, whole, false, source, m_sampler, uniforms,
                          "downscale", error);
}

bool SdlGpuDevice::drawRcas(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *target, SDL_GPUTextureFormat format,
                            const uint32_t (&rect)[4], SDL_GPUTexture *source, float strength, bool clear,
                            std::string &error) {
    if ((SDL_GetGPUShaderFormats(m_device) & SDL_GPU_SHADERFORMAT_SPIRV) == 0) {
        error = "no SPIR-V shaders on this device";
        return false;
    }
    if (m_rcasPipeline && m_rcasFormat != format) {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_rcasPipeline);
        m_rcasPipeline = nullptr;
    }
    if (!m_rcasPipeline) {
        m_rcasPipeline = createPresentPass(m_device, kPresentRcasFragSpirv, format, "rcas", error);
        if (!m_rcasPipeline)
            return false;
        m_rcasFormat = format;
    }
    const float uniforms[8] = {float(rect[0]), float(rect[1]), float(rect[2]), float(rect[3]),
                               strength,       0.0f,           0.0f,           0.0f};
    return runPresentPass(commands, m_rcasPipeline, target, rect, clear, source, m_sampler, uniforms, "rcas", error);
}

} // namespace dq8::gfx
