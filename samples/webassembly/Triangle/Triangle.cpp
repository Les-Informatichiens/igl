/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#ifndef __EMSCRIPTEN__
#error Unsupported OS
#endif
#include <emscripten.h>

#include <cassert>
#include <regex>
#include <stdio.h>

#include <igl/IGL.h>
#include <igl/opengl/webgl/Device.h>
#include <igl/opengl/webgl/Context.h>
// clang-format on

#define ENABLE_MULTIPLE_COLOR_ATTACHMENTS 0

#if ENABLE_MULTIPLE_COLOR_ATTACHMENTS
static const uint32_t kNumColorAttachments = 4;
#else
static const uint32_t kNumColorAttachments = 1;
#endif

#if defined(__cpp_lib_format)
#include <format>
#define IGL_FORMAT std::format
#else
#include <fmt/core.h>
#define IGL_FORMAT fmt::format
#endif // __cpp_lib_format

std::string codeVS = R"(#version 300 es

precision highp float;

out vec3 vColor;
const vec2 pos[3] = vec2[3](
	vec2(-0.6, -0.4),
	vec2( 0.6, -0.4),
	vec2( 0.0,  0.6)
);
const vec3 col[3] = vec3[3](
	vec3(1.0, 0.0, 0.0),
	vec3(0.0, 1.0, 0.0),
	vec3(0.0, 0.0, 1.0)
);
void main() {
	gl_Position = vec4(pos[gl_VertexID], 0.0, 1.0);
	vColor = col[gl_VertexID];
}
)";

const char* codeFS = R"(#version 300 es

precision highp float;

in vec3 vColor;
layout (location=0) out vec4 oColor;
void main() {
	oColor = vec4(vColor, 1.0);
}
)";

using namespace igl;

GLFWwindow* window_ = nullptr;
int width_ = 0;
int height_ = 0;

std::unique_ptr<IDevice> device_;
std::shared_ptr<ICommandQueue> commandQueue_;
RenderPassDesc renderPass_;
std::shared_ptr<IFramebuffer> framebuffer_;
std::shared_ptr<IRenderPipelineState> renderPipelineState_Triangle_;

static bool initWindow(GLFWwindow** outWindow) {
  if (!glfwInit()) {
    return false;
  }

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);
  glfwWindowHint(GLFW_VISIBLE, true);
  glfwWindowHint(GLFW_DOUBLEBUFFER, true);
  glfwWindowHint(GLFW_SRGB_CAPABLE, true);
  glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
  glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

  GLFWwindow* window = glfwCreateWindow(800, 600, "WebGL Triangle", nullptr, nullptr);

  if (!window) {
    glfwTerminate();
    return false;
  }

  glfwSetErrorCallback([](int error, const char* description) {
    printf("GLFW Error (%i): %s\n", error, description);
  });

  glfwSetKeyCallback(window, [](GLFWwindow* window, int key, int, int action, int) {
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
      glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
  });

  // @lint-ignore CLANGTIDY
  glfwSetWindowSizeCallback(window, [](GLFWwindow* /*window*/, int width, int height) {
    printf("Window resized! width=%d, height=%d\n", width, height);
    width_ = width;
    height_ = height;
  });

  glfwGetWindowSize(window, &width_, &height_);

  if (outWindow) {
    *outWindow = window;
  }

  return true;
}

static void initIGL() {
  // create a device
  {
    auto ctx = std::make_unique<igl::opengl::webgl::Context>(igl::opengl::RenderingAPI::GLES3, "#canvas");
    device_ = std::make_unique<igl::opengl::webgl::Device>(std::move(ctx));

    IGL_ASSERT(device_);
  }

  // Command queue: backed by different types of GPU HW queues
  CommandQueueDesc desc{CommandQueueType::Graphics};
  commandQueue_ = device_->createCommandQueue(desc, nullptr);

  // first color attachment
  for (auto i = 0; i < kNumColorAttachments; ++i) {
    // Generate sparse color attachments by skipping alternate slots
    if (i & 0x1) {
      continue;
    }
    renderPass_.colorAttachments[i] = igl::RenderPassDesc::ColorAttachmentDesc{};
    renderPass_.colorAttachments[i].loadAction = LoadAction::Clear;
    renderPass_.colorAttachments[i].storeAction = StoreAction::Store;
    renderPass_.colorAttachments[i].clearColor = {1.0f, 1.0f, 1.0f, 1.0f};
  }
  renderPass_.depthAttachment.loadAction = LoadAction::DontCare;
}

static void createRenderPipeline() {
  if (renderPipelineState_Triangle_) {
    return;
  }

  IGL_ASSERT(framebuffer_);

  RenderPipelineDesc desc;

  desc.targetDesc.colorAttachments.resize(kNumColorAttachments);

  for (auto i = 0; i < kNumColorAttachments; ++i) {
    // @fb-only
    if (framebuffer_->getColorAttachment(i)) {
      desc.targetDesc.colorAttachments[i].textureFormat =
          framebuffer_->getColorAttachment(i)->getFormat();
    }
  }

  if (framebuffer_->getDepthAttachment()) {
    desc.targetDesc.depthAttachmentFormat = framebuffer_->getDepthAttachment()->getFormat();
  }

  desc.shaderStages = ShaderStagesCreator::fromModuleStringInput(
      *device_, codeVS.c_str(), "main", "", codeFS, "main", "", nullptr);
  renderPipelineState_Triangle_ = device_->createRenderPipeline(desc, nullptr);
  IGL_ASSERT(renderPipelineState_Triangle_);
}

static std::shared_ptr<ITexture> getNativeDrawable() {
  Result ret;
  std::shared_ptr<ITexture> drawable;
  const auto& platformDevice = device_->getPlatformDevice<opengl::webgl::PlatformDevice>();
  IGL_ASSERT(platformDevice != nullptr);
  drawable = platformDevice->createTextureFromNativeDrawable(&ret);

  IGL_ASSERT_MSG(ret.isOk(), ret.message.c_str());
  IGL_ASSERT(drawable != nullptr);
  return drawable;
}

static void createFramebuffer(const std::shared_ptr<ITexture>& nativeDrawable) {
  FramebufferDesc framebufferDesc;
  framebufferDesc.colorAttachments[0].texture = nativeDrawable;

  for (auto i = 1; i < kNumColorAttachments; ++i) {
    // Generate sparse color attachments by skipping alternate slots
    if (i & 0x1) {
      continue;
    }
    const TextureDesc desc = TextureDesc::new2D(
        nativeDrawable->getFormat(),
        nativeDrawable->getDimensions().width,
        nativeDrawable->getDimensions().height,
        TextureDesc::TextureUsageBits::Attachment | TextureDesc::TextureUsageBits::Sampled,
        IGL_FORMAT("{}C{}", framebufferDesc.debugName.c_str(), i - 1).c_str());

    framebufferDesc.colorAttachments[i].texture = device_->createTexture(desc, nullptr);
  }
  framebuffer_ = device_->createFramebuffer(framebufferDesc, nullptr);
  IGL_ASSERT(framebuffer_);
}

static void render(const std::shared_ptr<ITexture>& nativeDrawable) {
  const auto size = framebuffer_->getColorAttachment(0)->getSize();
  if (size.width != width_ || size.height != height_) {
    createFramebuffer(nativeDrawable);
  } else {
    framebuffer_->updateDrawable(nativeDrawable);
  }

  // Command buffers (1-N per thread): create, submit and forget
  CommandBufferDesc cbDesc;
  std::shared_ptr<ICommandBuffer> buffer = commandQueue_->createCommandBuffer(cbDesc, nullptr);

  const igl::Viewport viewport = {0.0f, 0.0f, (float)width_, (float)height_, 0.0f, +1.0f};
  const igl::ScissorRect scissor = {0, 0, (uint32_t)width_, (uint32_t)height_};

  // This will clear the framebuffer
  auto commands = buffer->createRenderCommandEncoder(renderPass_, framebuffer_);

  commands->bindRenderPipelineState(renderPipelineState_Triangle_);
  commands->bindViewport(viewport);
  commands->bindScissorRect(scissor);
  commands->pushDebugGroupLabel("Render Triangle", igl::Color(1, 0, 0));
  commands->draw(PrimitiveType::Triangle, 0, 3);
  commands->popDebugGroupLabel();
  commands->endEncoding();

  buffer->present(nativeDrawable);

  commandQueue_->submit(*buffer);
}

void mainLoop()
{
    render(getNativeDrawable());
    glfwPollEvents();
}

int main(int argc, char* argv[]) {
  renderPass_.colorAttachments.resize(kNumColorAttachments);
  initWindow(&window_);
  initIGL();

  createFramebuffer(getNativeDrawable());
  createRenderPipeline();

  // Main loop
  emscripten_set_main_loop(&mainLoop, 0, 1);
  
  // destroy all the Vulkan stuff before closing the window
  renderPipelineState_Triangle_ = nullptr;
  framebuffer_ = nullptr;
  device_.reset(nullptr);

  glfwDestroyWindow(window_);
  glfwTerminate();

  return 0;
}
