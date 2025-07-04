/* Copyright (c) 2021-2025, NVIDIA CORPORATION. All rights reserved.
 * Copyright (c) 2024-2025, Arm Limited and Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 the "License";
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "hpp_hello_triangle.h"

#include <common/hpp_error.h>
#include <common/hpp_vk_common.h>
#include <core/util/logging.hpp>
#include <filesystem/legacy.h>
#include <platform/window.h>

// Note: the default dispatcher is instantiated in hpp_api_vulkan_sample.cpp.
//			 Even though, that file is not part of this sample, it's part of the sample-project!

#if defined(VKB_DEBUG) || defined(VKB_VALIDATION_LAYERS)
/// @brief A debug callback called from Vulkan validation layers.
VKAPI_ATTR vk::Bool32 VKAPI_CALL debug_utils_messenger_callback(vk::DebugUtilsMessageSeverityFlagBitsEXT      message_severity,
                                                                vk::DebugUtilsMessageTypeFlagsEXT             message_type,
                                                                const vk::DebugUtilsMessengerCallbackDataEXT *callback_data,
                                                                void                                         *user_data)
{
	// Log debug message
	if (message_severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning)
	{
		LOGW("{} - {}: {}", callback_data->messageIdNumber, callback_data->pMessageIdName, callback_data->pMessage);
	}
	else if (message_severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eError)
	{
		LOGE("{} - {}: {}", callback_data->messageIdNumber, callback_data->pMessageIdName, callback_data->pMessage);
	}
	return VK_FALSE;
}
#endif

/**
 * @brief Validates a list of required extensions, comparing it with the available ones.
 *
 * @param required A vector containing required extension names.
 * @param available A vk::ExtensionProperties object containing available extensions.
 * @return true if all required extensions are available
 * @return false otherwise
 */
bool validate_extensions(const std::vector<const char *>            &required,
                         const std::vector<vk::ExtensionProperties> &available)
{
	// inner find_if gives true if the extension was not found
	// outer find_if gives true if none of the extensions were not found, that is if all extensions were found
	return std::ranges::find_if(required,
	                            [&available](auto extension) {
		                            return std::ranges::find_if(available,
		                                                        [&extension](auto const &ep) {
			                                                        return strcmp(ep.extensionName, extension) == 0;
		                                                        }) == available.end();
	                            }) == required.end();
}

HPPHelloTriangle::HPPHelloTriangle()
{
}

HPPHelloTriangle::~HPPHelloTriangle()
{
	// Don't release anything until the GPU is completely idle.
	device.waitIdle();

	teardown_framebuffers();

	for (auto &pfd : per_frame_data)
	{
		teardown_per_frame(pfd);
	}

	per_frame_data.clear();

	for (auto semaphore : recycled_semaphores)
	{
		device.destroySemaphore(semaphore);
	}

	if (pipeline)
	{
		device.destroyPipeline(pipeline);
	}

	if (pipeline_layout)
	{
		device.destroyPipelineLayout(pipeline_layout);
	}

	if (render_pass)
	{
		device.destroyRenderPass(render_pass);
	}

	for (auto image_view : swapchain_data.image_views)
	{
		device.destroyImageView(image_view);
	}

	if (swapchain_data.swapchain)
	{
		device.destroySwapchainKHR(swapchain_data.swapchain);
	}

	if (surface)
	{
		instance.destroySurfaceKHR(surface);
	}

	if (device)
	{
		device.destroy();
	}

	if (debug_utils_messenger)
	{
		instance.destroyDebugUtilsMessengerEXT(debug_utils_messenger);
	}

	instance.destroy();
}

bool HPPHelloTriangle::prepare(const vkb::ApplicationOptions &options)
{
	// Headless is not supported to keep this sample as simple as possible
	assert(options.window != nullptr);
	assert(options.window->get_window_mode() != vkb::Window::Mode::Headless);

	if (Application::prepare(options))
	{
		instance = create_instance({VK_KHR_SURFACE_EXTENSION_NAME}, {});
#if defined(VKB_DEBUG) || defined(VKB_VALIDATION_LAYERS)
		debug_utils_messenger = instance.createDebugUtilsMessengerEXT(debug_utils_create_info);
#endif

		select_physical_device_and_surface();

		const vkb::Window::Extent &extent = options.window->get_extent();
		swapchain_data.extent.width       = extent.width;
		swapchain_data.extent.height      = extent.height;

		// create a device
		device = create_device({VK_KHR_SWAPCHAIN_EXTENSION_NAME});

		// get the (graphics) queue
		queue = device.getQueue(graphics_queue_index, 0);

		init_swapchain();

		// Create the necessary objects for rendering.
		render_pass = create_render_pass();

		// Create a blank pipeline layout.
		// We are not binding any resources to the pipeline in this first sample.
		pipeline_layout = device.createPipelineLayout({});

		pipeline = create_graphics_pipeline();

		init_framebuffers();
	}

	return true;
}

void HPPHelloTriangle::update(float delta_time)
{
	vk::Result res;
	uint32_t   index;
	std::tie(res, index) = acquire_next_image();

	// Handle outdated error in acquire.
	if (res == vk::Result::eSuboptimalKHR || res == vk::Result::eErrorOutOfDateKHR)
	{
		resize(swapchain_data.extent.width, swapchain_data.extent.height);
		std::tie(res, index) = acquire_next_image();
	}

	if (res != vk::Result::eSuccess)
	{
		queue.waitIdle();
		return;
	}

	render_triangle(index);

	// Present swapchain image
	vk::PresentInfoKHR present_info{.waitSemaphoreCount = 1,
	                                .pWaitSemaphores    = &per_frame_data[index].swapchain_release_semaphore,
	                                .swapchainCount     = 1,
	                                .pSwapchains        = &swapchain_data.swapchain,
	                                .pImageIndices      = &index};
	res = queue.presentKHR(present_info);

	// Handle Outdated error in present.
	if (res == vk::Result::eSuboptimalKHR || res == vk::Result::eErrorOutOfDateKHR)
	{
		resize(swapchain_data.extent.width, swapchain_data.extent.height);
	}
	else if (res != vk::Result::eSuccess)
	{
		LOGE("Failed to present swapchain image.");
	}
}

bool HPPHelloTriangle::resize(const uint32_t, const uint32_t)
{
	if (!device)
	{
		return false;
	}

	vk::SurfaceCapabilitiesKHR surface_properties = gpu.getSurfaceCapabilitiesKHR(surface);

	// Only rebuild the swapchain if the dimensions have changed
	if (surface_properties.currentExtent == swapchain_data.extent)
	{
		return false;
	}

	device.waitIdle();
	teardown_framebuffers();

	init_swapchain();
	init_framebuffers();
	return true;
}

/**
 * @brief Acquires an image from the swapchain.
 * @param[out] image The swapchain index for the acquired image.
 * @returns Vulkan result code
 */
std::pair<vk::Result, uint32_t> HPPHelloTriangle::acquire_next_image()
{
	vk::Semaphore acquire_semaphore;
	if (recycled_semaphores.empty())
	{
		acquire_semaphore = device.createSemaphore({});
	}
	else
	{
		acquire_semaphore = recycled_semaphores.back();
		recycled_semaphores.pop_back();
	}

	vk::Result res;
	uint32_t   image;
	std::tie(res, image) = device.acquireNextImageKHR(swapchain_data.swapchain, UINT64_MAX, acquire_semaphore);

	if (res != vk::Result::eSuccess)
	{
		recycled_semaphores.push_back(acquire_semaphore);
		return {res, image};
	}

	// If we have outstanding fences for this swapchain image, wait for them to complete first.
	// After begin frame returns, it is safe to reuse or delete resources which
	// were used previously.
	//
	// We wait for fences which completes N frames earlier, so we do not stall,
	// waiting for all GPU work to complete before this returns.
	// Normally, this doesn't really block at all,
	// since we're waiting for old frames to have been completed, but just in case.
	if (per_frame_data[image].queue_submit_fence)
	{
		(void) device.waitForFences(per_frame_data[image].queue_submit_fence, true, UINT64_MAX);
		device.resetFences(per_frame_data[image].queue_submit_fence);
	}

	if (per_frame_data[image].primary_command_pool)
	{
		device.resetCommandPool(per_frame_data[image].primary_command_pool);
	}

	// Recycle the old semaphore back into the semaphore manager.
	vk::Semaphore old_semaphore = per_frame_data[image].swapchain_acquire_semaphore;

	if (old_semaphore)
	{
		recycled_semaphores.push_back(old_semaphore);
	}

	per_frame_data[image].swapchain_acquire_semaphore = acquire_semaphore;

	return {vk::Result::eSuccess, image};
}

vk::Device HPPHelloTriangle::create_device(const std::vector<const char *> &required_device_extensions)
{
	std::vector<vk::ExtensionProperties> device_extensions = gpu.enumerateDeviceExtensionProperties();

	if (!validate_extensions(required_device_extensions, device_extensions))
	{
		throw std::runtime_error("Required device extensions are missing, will try without.");
	}

	std::vector<const char *> active_device_extensions(required_device_extensions);

#if (defined(VKB_ENABLE_PORTABILITY))
	// VK_KHR_portability_subset must be enabled if present in the implementation (e.g on macOS/iOS with beta extensions enabled)
	if (std::ranges::any_of(device_extensions,
	                        [](vk::ExtensionProperties const &extension) { return strcmp(extension.extensionName, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME) == 0; }))
	{
		active_device_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
	}
#endif

	// Create a device with one queue
	float                     queue_priority = 1.0f;
	vk::DeviceQueueCreateInfo queue_info{.queueFamilyIndex = graphics_queue_index, .queueCount = 1, .pQueuePriorities = &queue_priority};
	vk::DeviceCreateInfo      device_info{.queueCreateInfoCount    = 1,
	                                      .pQueueCreateInfos       = &queue_info,
	                                      .enabledExtensionCount   = static_cast<uint32_t>(active_device_extensions.size()),
	                                      .ppEnabledExtensionNames = active_device_extensions.data()};
	vk::Device                device = gpu.createDevice(device_info);

	// initialize function pointers for device
	VULKAN_HPP_DEFAULT_DISPATCHER.init(device);

	return device;
}

vk::Pipeline HPPHelloTriangle::create_graphics_pipeline()
{
	// Load our SPIR-V shaders.
	std::vector<vk::PipelineShaderStageCreateInfo> shader_stages{
	    {.stage = vk::ShaderStageFlagBits::eVertex, .module = create_shader_module("triangle.vert.spv"), .pName = "main"},
	    {.stage = vk::ShaderStageFlagBits::eFragment, .module = create_shader_module("triangle.frag.spv"), .pName = "main"}};

	vk::PipelineVertexInputStateCreateInfo vertex_input;

	// Our attachment will write to all color channels, but no blending is enabled.
	vk::PipelineColorBlendAttachmentState blend_attachment{.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
	                                                                         vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};

	// Disable all depth testing.
	vk::PipelineDepthStencilStateCreateInfo depth_stencil;

	vk::Pipeline pipeline = vkb::common::create_graphics_pipeline(device,
	                                                              nullptr,
	                                                              shader_stages,
	                                                              vertex_input,
	                                                              vk::PrimitiveTopology::eTriangleList,        // We will use triangle lists to draw geometry.
	                                                              0,
	                                                              vk::PolygonMode::eFill,
	                                                              vk::CullModeFlagBits::eBack,
	                                                              vk::FrontFace::eClockwise,
	                                                              {blend_attachment},
	                                                              depth_stencil,
	                                                              pipeline_layout,        // We need to specify the pipeline layout
	                                                              render_pass);           // and the render pass up front as well

	// Pipeline is baked, we can delete the shader modules now.
	device.destroyShaderModule(shader_stages[0].module);
	device.destroyShaderModule(shader_stages[1].module);

	return pipeline;
}

vk::ImageView HPPHelloTriangle::create_image_view(vk::Image image)
{
	vk::ImageViewCreateInfo image_view_create_info{
	    .image            = image,
	    .viewType         = vk::ImageViewType::e2D,
	    .format           = swapchain_data.format,
	    .components       = {vk::ComponentSwizzle::eR, vk::ComponentSwizzle::eG, vk::ComponentSwizzle::eB, vk::ComponentSwizzle::eA},
	    .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}};
	return device.createImageView(image_view_create_info);
}

vk::Instance HPPHelloTriangle::create_instance(std::vector<const char *> const &required_instance_extensions, std::vector<const char *> const &required_validation_layers)
{
#if defined(_HPP_VULKAN_LIBRARY)
	static vk::detail::DynamicLoader dl(_HPP_VULKAN_LIBRARY);
#else
	static vk::detail::DynamicLoader dl;
#endif
	PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = dl.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
	VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);

	std::vector<vk::ExtensionProperties> available_instance_extensions = vk::enumerateInstanceExtensionProperties();

	std::vector<const char *> active_instance_extensions(required_instance_extensions);

#if defined(VKB_DEBUG) || defined(VKB_VALIDATION_LAYERS)
	active_instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif

#if (defined(VKB_ENABLE_PORTABILITY))
	active_instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
	bool portability_enumeration_available = false;
	if (std::ranges::any_of(available_instance_extensions,
	                        [](vk::ExtensionProperties const &extension) { return strcmp(extension.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0; }))
	{
		active_instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
		portability_enumeration_available = true;
	}
#endif

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
	active_instance_extensions.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_WIN32_KHR)
	active_instance_extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_METAL_EXT)
	active_instance_extensions.push_back(VK_EXT_METAL_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_XCB_KHR)
	active_instance_extensions.push_back(VK_KHR_XCB_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
	active_instance_extensions.push_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
	active_instance_extensions.push_back(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_DISPLAY_KHR)
	active_instance_extensions.push_back(VK_KHR_DISPLAY_EXTENSION_NAME);
#else
#	pragma error Platform not supported
#endif

	if (!validate_extensions(active_instance_extensions, available_instance_extensions))
	{
		throw std::runtime_error("Required instance extensions are missing.");
	}

	std::vector<const char *> requested_instance_layers(required_validation_layers);

#if defined(VKB_DEBUG) || defined(VKB_VALIDATION_LAYERS)
	char const *validationLayer = "VK_LAYER_KHRONOS_validation";

	std::vector<vk::LayerProperties> supported_instance_layers = vk::enumerateInstanceLayerProperties();

	if (std::ranges::any_of(supported_instance_layers, [&validationLayer](auto const &lp) { return strcmp(lp.layerName, validationLayer) == 0; }))
	{
		requested_instance_layers.push_back(validationLayer);
		LOGI("Enabled Validation Layer {}", validationLayer);
	}
	else
	{
		LOGW("Validation Layer {} is not available", validationLayer);
	}
#endif

	vk::ApplicationInfo app{.pApplicationName = "HPP Hello Triangle", .pEngineName = "Vulkan Samples", .apiVersion = VK_API_VERSION_1_1};

	vk::InstanceCreateInfo instance_info{.pApplicationInfo        = &app,
	                                     .enabledLayerCount       = static_cast<uint32_t>(requested_instance_layers.size()),
	                                     .ppEnabledLayerNames     = requested_instance_layers.data(),
	                                     .enabledExtensionCount   = static_cast<uint32_t>(active_instance_extensions.size()),
	                                     .ppEnabledExtensionNames = active_instance_extensions.data()};

#if defined(VKB_DEBUG) || defined(VKB_VALIDATION_LAYERS)
	debug_utils_create_info =
	    vk::DebugUtilsMessengerCreateInfoEXT{.messageSeverity =
	                                             vk::DebugUtilsMessageSeverityFlagBitsEXT::eError | vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning,
	                                         .messageType     = vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation | vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
	                                         .pfnUserCallback = debug_utils_messenger_callback};

	instance_info.pNext = &debug_utils_create_info;
#endif

#if (defined(VKB_ENABLE_PORTABILITY))
	if (portability_enumeration_available)
	{
		instance_info.flags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
	}
#endif

	// Create the Vulkan instance
	vk::Instance instance = vk::createInstance(instance_info);

	// initialize function pointers for instance
	VULKAN_HPP_DEFAULT_DISPATCHER.init(instance);

#if defined(VK_USE_PLATFORM_DISPLAY_KHR) || defined(VK_USE_PLATFORM_ANDROID_KHR) || defined(VK_USE_PLATFORM_METAL_EXT)
	// we need some additional initializing for this platform!
	if (volkInitialize())
	{
		throw std::runtime_error("Failed to initialize volk.");
	}
	volkLoadInstance(instance);
#endif

	return instance;
}

vk::RenderPass HPPHelloTriangle::create_render_pass()
{
	vk::AttachmentDescription attachment{{},
	                                     swapchain_data.format,                  // Backbuffer format
	                                     vk::SampleCountFlagBits::e1,            // Not multisampled
	                                     vk::AttachmentLoadOp::eClear,           // When starting the frame, we want tiles to be cleared
	                                     vk::AttachmentStoreOp::eStore,          // When ending the frame, we want tiles to be written out
	                                     vk::AttachmentLoadOp::eDontCare,        // Don't care about stencil since we're not using it
	                                     vk::AttachmentStoreOp::eDontCare,
	                                     vk::ImageLayout::eUndefined,             // The image layout will be undefined when the render pass begins
	                                     vk::ImageLayout::ePresentSrcKHR};        // After the render pass is complete, we will transition to ePresentSrcKHR layout

	// We have one subpass. This subpass has one color attachment.
	// While executing this subpass, the attachment will be in attachment optimal layout.
	vk::AttachmentReference color_ref{0, vk::ImageLayout::eColorAttachmentOptimal};

	// We will end up with two transitions.
	// The first one happens right before we start subpass #0, where
	// eUndefined is transitioned into eColorAttachmentOptimal.
	// The final layout in the render pass attachment states ePresentSrcKHR, so we
	// will get a final transition from eColorAttachmentOptimal to ePresetSrcKHR.
	vk::SubpassDescription subpass{.pipelineBindPoint = vk::PipelineBindPoint::eGraphics, .colorAttachmentCount = 1, .pColorAttachments = &color_ref};

	// Create a dependency to external events.
	// We need to wait for the WSI semaphore to signal.
	// Only pipeline stages which depend on eColorAttachmentOutput will
	// actually wait for the semaphore, so we must also wait for that pipeline stage.
	vk::SubpassDependency dependency{.srcSubpass   = vk::SubpassExternal,
	                                 .dstSubpass   = 0,
	                                 .srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput,
	                                 .dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput,
	                                 // Since we changed the image layout, we need to make the memory visible to color attachment to modify.
	                                 .srcAccessMask = {},
	                                 .dstAccessMask = vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite};

	// Finally, create the renderpass.
	vk::RenderPassCreateInfo rp_info{
	    .attachmentCount = 1, .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass, .dependencyCount = 1, .pDependencies = &dependency};
	return device.createRenderPass(rp_info);
}

/**
 * @brief Helper function to load a shader module.
 * @param path The path for the shader (relative to the assets directory).
 * @returns A vk::ShaderModule handle. Aborts execution if shader creation fails.
 */
vk::ShaderModule HPPHelloTriangle::create_shader_module(const char *path)
{
	auto spirv = vkb::fs::read_shader_binary_u32(path);

	VkShaderModuleCreateInfo module_info{
	    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	    .codeSize = spirv.size() * sizeof(uint32_t),
	    .pCode    = spirv.data()};

	return device.createShaderModule({.codeSize = static_cast<uint32_t>(spirv.size() * sizeof(uint32_t)), .pCode = spirv.data()});
}

vk::SwapchainKHR
    HPPHelloTriangle::create_swapchain(vk::Extent2D const &swapchain_extent, vk::SurfaceFormatKHR surface_format, vk::SwapchainKHR old_swapchain)
{
	vk::SurfaceCapabilitiesKHR surface_properties = gpu.getSurfaceCapabilitiesKHR(surface);

	// Determine the number of vk::Image's to use in the swapchain.
	// Ideally, we desire to own 1 image at a time, the rest of the images can
	// either be rendered to and/or being queued up for display.
	uint32_t desired_swapchain_images = surface_properties.minImageCount + 1;
	if ((surface_properties.maxImageCount > 0) && (desired_swapchain_images > surface_properties.maxImageCount))
	{
		// Application must settle for fewer images than desired.
		desired_swapchain_images = surface_properties.maxImageCount;
	}

	// Figure out a suitable surface transform.
	vk::SurfaceTransformFlagBitsKHR pre_transform =
	    (surface_properties.supportedTransforms & vk::SurfaceTransformFlagBitsKHR::eIdentity) ? vk::SurfaceTransformFlagBitsKHR::eIdentity : surface_properties.currentTransform;

	// Find a supported composite type.
	vk::CompositeAlphaFlagBitsKHR composite = vk::CompositeAlphaFlagBitsKHR::eOpaque;
	if (surface_properties.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::eOpaque)
	{
		composite = vk::CompositeAlphaFlagBitsKHR::eOpaque;
	}
	else if (surface_properties.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::eInherit)
	{
		composite = vk::CompositeAlphaFlagBitsKHR::eInherit;
	}
	else if (surface_properties.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::ePreMultiplied)
	{
		composite = vk::CompositeAlphaFlagBitsKHR::ePreMultiplied;
	}
	else if (surface_properties.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::ePostMultiplied)
	{
		composite = vk::CompositeAlphaFlagBitsKHR::ePostMultiplied;
	}

	// FIFO must be supported by all implementations.
	vk::PresentModeKHR swapchain_present_mode = vk::PresentModeKHR::eFifo;

	vk::SwapchainCreateInfoKHR swapchain_create_info;
	swapchain_create_info.surface            = surface;
	swapchain_create_info.minImageCount      = desired_swapchain_images;
	swapchain_create_info.imageFormat        = surface_format.format;
	swapchain_create_info.imageColorSpace    = surface_format.colorSpace;
	swapchain_create_info.imageExtent.width  = swapchain_extent.width;
	swapchain_create_info.imageExtent.height = swapchain_extent.height;
	swapchain_create_info.imageArrayLayers   = 1;
	swapchain_create_info.imageUsage         = vk::ImageUsageFlagBits::eColorAttachment;
	swapchain_create_info.imageSharingMode   = vk::SharingMode::eExclusive;
	swapchain_create_info.preTransform       = pre_transform;
	swapchain_create_info.compositeAlpha     = composite;
	swapchain_create_info.presentMode        = swapchain_present_mode;
	swapchain_create_info.clipped            = true;
	swapchain_create_info.oldSwapchain       = old_swapchain;

	return device.createSwapchainKHR(swapchain_create_info);
}

/**
 * @brief Initializes the Vulkan framebuffers.
 */
void HPPHelloTriangle::init_framebuffers()
{
	assert(swapchain_data.framebuffers.empty());

	// Create framebuffer for each swapchain image view
	for (auto &image_view : swapchain_data.image_views)
	{
		// create the framebuffer.
		swapchain_data.framebuffers.push_back(vkb::common::create_framebuffer(device, render_pass, {image_view}, swapchain_data.extent));
	}
}

/**
 * @brief Initializes the Vulkan swapchain.
 */
void HPPHelloTriangle::init_swapchain()
{
	vk::SurfaceCapabilitiesKHR surface_properties = gpu.getSurfaceCapabilitiesKHR(surface);

	vk::Extent2D swapchain_extent = (surface_properties.currentExtent.width == 0xFFFFFFFF) ? swapchain_data.extent : surface_properties.currentExtent;

	vk::SurfaceFormatKHR surface_format = vkb::common::select_surface_format(gpu, surface);

	vk::SwapchainKHR old_swapchain = swapchain_data.swapchain;

	swapchain_data.swapchain = create_swapchain(swapchain_extent, surface_format, old_swapchain);

	if (old_swapchain)
	{
		for (vk::ImageView image_view : swapchain_data.image_views)
		{
			device.destroyImageView(image_view);
		}

		size_t image_count = device.getSwapchainImagesKHR(old_swapchain).size();

		for (size_t i = 0; i < image_count; i++)
		{
			teardown_per_frame(per_frame_data[i]);
		}

		swapchain_data.image_views.clear();

		device.destroySwapchainKHR(old_swapchain);
	}

	swapchain_data.extent = swapchain_extent;
	swapchain_data.format = surface_format.format;

	/// The swapchain images.
	std::vector<vk::Image> swapchain_images = device.getSwapchainImagesKHR(swapchain_data.swapchain);
	size_t                 image_count      = swapchain_images.size();

	// Initialize per-frame resources.
	// Every swapchain image has its own command pool and fence manager.
	// This makes it very easy to keep track of when we can reset command buffers and such.
	per_frame_data.clear();
	per_frame_data.resize(image_count);

	for (size_t frame = 0; frame < image_count; frame++)
	{
		auto &pfd                  = per_frame_data[frame];
		pfd.queue_submit_fence     = device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled});
		pfd.primary_command_pool   = device.createCommandPool({.flags = vk::CommandPoolCreateFlagBits::eTransient, .queueFamilyIndex = graphics_queue_index});
		pfd.primary_command_buffer = vkb::common::allocate_command_buffer(device, pfd.primary_command_pool);
	}

	for (size_t i = 0; i < image_count; i++)
	{
		// Create an image view which we can render into.
		swapchain_data.image_views.push_back(create_image_view(swapchain_images[i]));
	}
}

/**
 * @brief Renders a triangle to the specified swapchain image.
 * @param swapchain_index The swapchain index for the image being rendered.
 */
void HPPHelloTriangle::render_triangle(uint32_t swapchain_index)
{
	// Render to this framebuffer.
	vk::Framebuffer framebuffer = swapchain_data.framebuffers[swapchain_index];

	// Allocate or re-use a primary command buffer.
	vk::CommandBuffer cmd = per_frame_data[swapchain_index].primary_command_buffer;

	// We will only submit this once before it's recycled.
	vk::CommandBufferBeginInfo begin_info{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit};
	// Begin command recording
	cmd.begin(begin_info);

	// Set clear color values.
	vk::ClearValue clear_value;
	clear_value.color = vk::ClearColorValue(std::array<float, 4>({{0.01f, 0.01f, 0.033f, 1.0f}}));

	// Begin the render pass.
	vk::RenderPassBeginInfo rp_begin{.renderPass      = render_pass,
	                                 .framebuffer     = framebuffer,
	                                 .renderArea      = {{0, 0}, {swapchain_data.extent.width, swapchain_data.extent.height}},
	                                 .clearValueCount = 1,
	                                 .pClearValues    = &clear_value};
	// We will add draw commands in the same command buffer.
	cmd.beginRenderPass(rp_begin, vk::SubpassContents::eInline);

	// Bind the graphics pipeline.
	cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

	vk::Viewport vp{0.0f, 0.0f, static_cast<float>(swapchain_data.extent.width), static_cast<float>(swapchain_data.extent.height), 0.0f, 1.0f};
	// Set viewport dynamically
	cmd.setViewport(0, vp);

	vk::Rect2D scissor{{0, 0}, {swapchain_data.extent.width, swapchain_data.extent.height}};
	// Set scissor dynamically
	cmd.setScissor(0, scissor);

	// Draw three vertices with one instance.
	cmd.draw(3, 1, 0, 0);

	// Complete render pass.
	cmd.endRenderPass();

	// Complete the command buffer.
	cmd.end();

	// Submit it to the queue with a release semaphore.
	if (!per_frame_data[swapchain_index].swapchain_release_semaphore)
	{
		per_frame_data[swapchain_index].swapchain_release_semaphore = device.createSemaphore({});
	}

	vk::PipelineStageFlags wait_stage{VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};

	vk::SubmitInfo info{.waitSemaphoreCount   = 1,
	                    .pWaitSemaphores      = &per_frame_data[swapchain_index].swapchain_acquire_semaphore,
	                    .pWaitDstStageMask    = &wait_stage,
	                    .commandBufferCount   = 1,
	                    .pCommandBuffers      = &cmd,
	                    .signalSemaphoreCount = 1,
	                    .pSignalSemaphores    = &per_frame_data[swapchain_index].swapchain_release_semaphore};
	// Submit command buffer to graphics queue
	queue.submit(info, per_frame_data[swapchain_index].queue_submit_fence);
}

/**
 * @brief Select a physical device.
 */
void HPPHelloTriangle::select_physical_device_and_surface()
{
	std::vector<vk::PhysicalDevice> gpus = instance.enumeratePhysicalDevices();

	bool found_graphics_queue_index = false;
	for (size_t i = 0; i < gpus.size() && !found_graphics_queue_index; i++)
	{
		gpu = gpus[i];

		std::vector<vk::QueueFamilyProperties> queue_family_properties = gpu.getQueueFamilyProperties();

		if (queue_family_properties.empty())
		{
			throw std::runtime_error("No queue family found.");
		}

		if (surface)
		{
			instance.destroySurfaceKHR(surface);
		}

		surface = static_cast<vk::SurfaceKHR>(window->create_surface(static_cast<VkInstance>(instance), static_cast<VkPhysicalDevice>(gpu)));
		if (!surface)
		{
			throw std::runtime_error("Failed to create window surface.");
		}

		for (uint32_t j = 0; j < vkb::to_u32(queue_family_properties.size()); j++)
		{
			vk::Bool32 supports_present = gpu.getSurfaceSupportKHR(j, surface);

			// Find a queue family which supports graphics and presentation.
			if ((queue_family_properties[j].queueFlags & vk::QueueFlagBits::eGraphics) && supports_present)
			{
				graphics_queue_index       = j;
				found_graphics_queue_index = true;
				break;
			}
		}
	}

	if (!found_graphics_queue_index)
	{
		LOGE("Did not find suitable queue which supports graphics and presentation.");
	}
}

/**
 * @brief Tears down the framebuffers. If our swapchain changes, we will call this, and create a new swapchain.
 */
void HPPHelloTriangle::teardown_framebuffers()
{
	// Wait until device is idle before teardown.
	queue.waitIdle();

	for (auto &framebuffer : swapchain_data.framebuffers)
	{
		device.destroyFramebuffer(framebuffer);
	}

	swapchain_data.framebuffers.clear();
}

/**
 * @brief Tears down the frame data.
 * @param per_frame_data The data of a frame.
 */
void HPPHelloTriangle::teardown_per_frame(FrameData &per_frame_data)
{
	if (per_frame_data.queue_submit_fence)
	{
		device.destroyFence(per_frame_data.queue_submit_fence);
		per_frame_data.queue_submit_fence = nullptr;
	}

	if (per_frame_data.primary_command_buffer)
	{
		device.freeCommandBuffers(per_frame_data.primary_command_pool, per_frame_data.primary_command_buffer);
		per_frame_data.primary_command_buffer = nullptr;
	}

	if (per_frame_data.primary_command_pool)
	{
		device.destroyCommandPool(per_frame_data.primary_command_pool);
		per_frame_data.primary_command_pool = nullptr;
	}

	if (per_frame_data.swapchain_acquire_semaphore)
	{
		device.destroySemaphore(per_frame_data.swapchain_acquire_semaphore);
		per_frame_data.swapchain_acquire_semaphore = nullptr;
	}

	if (per_frame_data.swapchain_release_semaphore)
	{
		device.destroySemaphore(per_frame_data.swapchain_release_semaphore);
		per_frame_data.swapchain_release_semaphore = nullptr;
	}
}

std::unique_ptr<vkb::Application> create_hpp_hello_triangle()
{
	return std::make_unique<HPPHelloTriangle>();
}
