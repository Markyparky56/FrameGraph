// Copyright (c) 2018-2019,  Zhirnov Andrey. For more information see 'LICENSE'
/*
	docs:
	https://github.com/KhronosGroup/GLSL/blob/master/extensions/nv/GLSL_NV_ray_tracing.txt
	https://github.com/KhronosGroup/Vulkan-Docs/tree/master/chapters/VK_NV_ray_tracing
	https://devblogs.nvidia.com/vulkan-raytracing/
	https://devblogs.nvidia.com/practical-real-time-ray-tracing-rtx/
	https://devblogs.nvidia.com/effectively-integrating-rtx-ray-tracing-real-time-rendering-engine/
	https://devblogs.nvidia.com/thinking-parallel-part-ii-tree-traversal-gpu/
	https://devblogs.nvidia.com/thinking-parallel-part-iii-tree-construction-gpu/

	this sample based on sample from https://github.com/iOrange/rtxON/tree/01_HappyTriangle
*/

#include "framework/Vulkan/VulkanDeviceExt.h"
#include "framework/Vulkan/VulkanSwapchain.h"
#include "framework/Window/WindowGLFW.h"
#include "framework/Window/WindowSDL2.h"
#include "framework/Window/WindowSFML.h"
#include "compiler/SpvCompiler.h"

namespace {

class RayTracingApp final : public IWindowEventListener, public VulkanDeviceFn
{
private:
	struct VkGeometryInstance
	{
		// 4x3 row-major matrix
		float4		transformRow0;
		float4		transformRow1;
		float4		transformRow2;

		uint		instanceId		: 24;
		uint		mask			: 8;
		uint		instanceOffset	: 24;
		uint		flags			: 8;
		uint64_t	accelerationStructureHandle;
	};

	struct MemInfo
	{
		VkDeviceSize			totalSize		= 0;
		uint					memTypeBits		= 0;
		VkMemoryPropertyFlags	memProperty		= 0;
	};

	struct ResourceInit
	{
		using BindMemCallbacks_t	= Array< std::function<bool (void *)> >;
		using DrawCallbacks_t		= Array< std::function<void (VkCommandBuffer)> >;

		MemInfo					host;
		MemInfo					dev;
		BindMemCallbacks_t		onBind;
		DrawCallbacks_t			onDraw;
	};

	enum {
		RAYGEN_SHADER,
		HIT_SHADER,
		MISS_SHADER,
		NUM_GROUPS
	};


private:
	VulkanDeviceExt				vulkan;
	VulkanSwapchainPtr			swapchain;
	WindowPtr					window;
	SpvCompiler					spvCompiler;
	
	VkCommandPool				cmdPool				= VK_NULL_HANDLE;
	VkQueue						cmdQueue			= VK_NULL_HANDLE;
	VkCommandBuffer				cmdBuffers[2]		= {};
	VkFence						fences[2]			= {};
	VkSemaphore					semaphores[2]		= {};

	VkShaderModule				rayGenShader		= VK_NULL_HANDLE;
	VkShaderModule				rayMissShader		= VK_NULL_HANDLE;
	VkShaderModule				rayClosestHitShader	= VK_NULL_HANDLE;
	VkPipelineLayout			pplnLayout			= VK_NULL_HANDLE;
	VkPipeline					rtPipeline			= VK_NULL_HANDLE;

	VkDescriptorSetLayout		dsLayout			= VK_NULL_HANDLE;
	VkDescriptorPool			descriptorPool		= VK_NULL_HANDLE;
	VkDescriptorSet				descriptorSet[2]	= {};

	VkAccelerationStructureNV	topLevelAS			= VK_NULL_HANDLE;
	VkAccelerationStructureNV	bottomLevelAS		= VK_NULL_HANDLE;
	uint64_t					bottomLevelASHandle	= 0;
	VkBuffer					vertexBuffer		= VK_NULL_HANDLE;
	VkBuffer					indexBuffer			= VK_NULL_HANDLE;
	VkBuffer					instanceBuffer		= VK_NULL_HANDLE;
	VkBuffer					scratchBuffer		= VK_NULL_HANDLE;
	VkBuffer					shaderBindingTable	= VK_NULL_HANDLE;
	VkDeviceMemory				sharedDevMemory		= VK_NULL_HANDLE;
	VkDeviceMemory				sharedHostMemory	= VK_NULL_HANDLE;

	bool						looping				= true;


public:
	RayTracingApp ()
	{
		VulkanDeviceFn_Init( vulkan );
	}
	
	void OnKey (StringView key, EKeyAction action) override;
	void OnResize (const uint2 &size) override;
	
	void OnRefresh () override {}
	void OnDestroy () override {}
	void OnUpdate () override {}
	void OnMouseMove (const float2 &) override {}

	bool Initialize ();
	void Destroy ();
	bool Run ();

	bool CreateCommandBuffers ();
	bool CreateSyncObjects ();
	bool CreateResources ();
	bool CreateDescriptorSet ();
	bool CreateRayTracingPipeline ();

	bool CreateBottomLevelAS (ResourceInit &);
	bool CreateTopLevelAS (ResourceInit &);
	bool CreateBindingTable (ResourceInit &);

	ND_ bool IsRayTracingSupported () const		{ return vulkan.HasDeviceExtension( VK_NV_RAY_TRACING_EXTENSION_NAME ); }
};



/*
=================================================
	OnKey
=================================================
*/
void RayTracingApp::OnKey (StringView key, EKeyAction action)
{
	if ( action != EKeyAction::Down )
		return;

	if ( key == "escape" )
		looping = false;
}

/*
=================================================
	OnResize
=================================================
*/
void RayTracingApp::OnResize (const uint2 &size)
{
	VK_CALL( vkDeviceWaitIdle( vulkan.GetVkDevice() ));

	VK_CALL( vkResetCommandPool( vulkan.GetVkDevice(), cmdPool, VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT ));

	CHECK( swapchain->Recreate( size ));
}

/*
=================================================
	Initialize
=================================================
*/
bool RayTracingApp::Initialize ()
{
# if defined(FG_ENABLE_GLFW)
	window.reset( new WindowGLFW() );

# elif defined(FG_ENABLE_SDL2)
	window.reset( new WindowSDL2() );

# elif defined(FG_ENABLE_SFML)
	window.reset( new WindowSFML() );

# else
#	error unknown window library!
# endif
	

	// create window and vulkan device
	{
		const char	title[] = "Ray tracing sample";

		CHECK_ERR( window->Create( { 800, 600 }, title ));
		window->AddListener( this );

		CHECK_ERR( vulkan.Create( window->GetVulkanSurface(),
								  title, "Engine",
								  VK_API_VERSION_1_1,
								  " RTX ",	// only RTX device is supported
								  {{ VK_QUEUE_PRESENT_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, 0.0f }},
								  VulkanDevice::GetRecomendedInstanceLayers(),
								  VulkanDevice::GetRecomendedInstanceExtensions(),
								  { VK_NV_RAY_TRACING_EXTENSION_NAME }
			));
		
		vulkan.CreateDebugUtilsCallback( DebugUtilsMessageSeverity_All );

		CHECK_ERR( IsRayTracingSupported() );
	}


	// initialize swapchain
	{
		VkFormat		color_fmt	= VK_FORMAT_UNDEFINED;
		VkColorSpaceKHR	color_space	= VK_COLOR_SPACE_MAX_ENUM_KHR;

		swapchain.reset( new VulkanSwapchain{ vulkan } );

		CHECK_ERR( swapchain->ChooseColorFormat( INOUT color_fmt, INOUT color_space ));

		CHECK_ERR( swapchain->Create( window->GetSize(), color_fmt, color_space ));
	}


	// initialize vulkan objects
	cmdQueue = vulkan.GetVkQueues().front().handle;

	CHECK_ERR( CreateCommandBuffers() );
	CHECK_ERR( CreateSyncObjects() );
	CHECK_ERR( CreateDescriptorSet() );
	CHECK_ERR( CreateRayTracingPipeline() );
	CHECK_ERR( CreateResources() );
	return true;
}

/*
=================================================
	Destroy
=================================================
*/
void RayTracingApp::Destroy ()
{
	VkDevice	dev = vulkan.GetVkDevice();

	VK_CALL( vkDeviceWaitIdle( dev ));
	
	for (auto& sem : semaphores) {
		vkDestroySemaphore( dev, sem, null );
		sem = VK_NULL_HANDLE;
	}
	for (auto& fen : fences) {
		vkDestroyFence( dev, fen, null );
		fen = VK_NULL_HANDLE;
	}
	vkDestroyCommandPool( dev, cmdPool, null );
	vkDestroyDescriptorSetLayout( dev, dsLayout, null );
	vkDestroyDescriptorPool( dev, descriptorPool, null );
	vkDestroyShaderModule( dev, rayGenShader, null );
	vkDestroyShaderModule( dev, rayMissShader, null );
	vkDestroyShaderModule( dev, rayClosestHitShader, null );
	vkDestroyPipelineLayout( dev, pplnLayout, null );
	vkDestroyPipeline( dev, rtPipeline, null );
	vkDestroyAccelerationStructureNV( dev, topLevelAS, null );
	vkDestroyAccelerationStructureNV( dev, bottomLevelAS, null );
	vkDestroyBuffer( dev, vertexBuffer, null );
	vkDestroyBuffer( dev, indexBuffer, null );
	vkDestroyBuffer( dev, instanceBuffer, null );
	vkDestroyBuffer( dev, scratchBuffer, null );
	vkDestroyBuffer( dev, shaderBindingTable, null );
	vkFreeMemory( dev, sharedDevMemory, null );
	vkFreeMemory( dev, sharedHostMemory, null );

	cmdPool				= VK_NULL_HANDLE;
	cmdQueue			= VK_NULL_HANDLE;
	dsLayout			= VK_NULL_HANDLE;
	descriptorPool		= VK_NULL_HANDLE;
	rayGenShader		= VK_NULL_HANDLE;
	rayMissShader		= VK_NULL_HANDLE;
	rayClosestHitShader	= VK_NULL_HANDLE;
	pplnLayout			= VK_NULL_HANDLE;
	rtPipeline			= VK_NULL_HANDLE;
	topLevelAS			= VK_NULL_HANDLE;
	bottomLevelAS		= VK_NULL_HANDLE;
	vertexBuffer		= VK_NULL_HANDLE;
	indexBuffer			= VK_NULL_HANDLE;
	instanceBuffer		= VK_NULL_HANDLE;
	scratchBuffer		= VK_NULL_HANDLE;
	shaderBindingTable	= VK_NULL_HANDLE;
	sharedDevMemory		= VK_NULL_HANDLE;
	sharedHostMemory	= VK_NULL_HANDLE;

	swapchain->Destroy();
	swapchain.reset();

	vulkan.Destroy();

	window->Destroy();
	window.reset();
}

/*
=================================================
	Run
=================================================
*/
bool RayTracingApp::Run ()
{
	for (uint frameId = 0; looping; frameId = ((frameId + 1) & 1))
	{
		if ( not window->Update() )
			break;

		VkCommandBuffer		cmd = cmdBuffers[frameId];

		// wait and acquire next image
		{
			VK_CHECK( vkWaitForFences( vulkan.GetVkDevice(), 1, &fences[frameId], true, UMax ));
			VK_CHECK( vkResetFences( vulkan.GetVkDevice(), 1, &fences[frameId] ));

			VK_CALL( swapchain->AcquireNextImage( semaphores[0] ));
		}

		// update descriptor set ('un_Output' only)
		{
			VkDescriptorImageInfo	images[1] = {};
			images[0].imageLayout	= VK_IMAGE_LAYOUT_GENERAL;
			images[0].imageView		= swapchain->GetCurrentImageView();
			images[0].sampler		= VK_NULL_HANDLE;

			VkWriteDescriptorSet	writes[1] = {};
			writes[0].sType				= VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[0].dstSet			= descriptorSet[frameId];
			writes[0].dstBinding		= 1;
			writes[0].descriptorCount	= 1;
			writes[0].descriptorType	= VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			writes[0].pImageInfo		= &images[0];

			vkUpdateDescriptorSets( vulkan.GetVkDevice(), uint(CountOf( writes )), writes, 0, null );
		}

		// build command buffer
		{
			VkCommandBufferBeginInfo	begin_info = {};
			begin_info.sType	= VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			begin_info.flags	= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			VK_CALL( vkBeginCommandBuffer( cmd, &begin_info ));
			
			// undefined -> general
			{
				VkImageMemoryBarrier	barrier = {};
				barrier.sType				= VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
				barrier.image				= swapchain->GetCurrentImage();
				barrier.oldLayout			= VK_IMAGE_LAYOUT_UNDEFINED;
				barrier.newLayout			= VK_IMAGE_LAYOUT_GENERAL;
				barrier.srcAccessMask		= VK_ACCESS_MEMORY_READ_BIT;
				barrier.dstAccessMask		= VK_ACCESS_SHADER_WRITE_BIT;
				barrier.srcQueueFamilyIndex	= VK_QUEUE_FAMILY_IGNORED;
				barrier.dstQueueFamilyIndex	= VK_QUEUE_FAMILY_IGNORED;
				barrier.subresourceRange	= { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

				vkCmdPipelineBarrier( cmd,
									  VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_NV, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_NV, 0,
									  0, null, 0, null, 1, &barrier );
			}

			// trace rays
			{
				vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_NV, rtPipeline );
				vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_NV, pplnLayout, 0, 1, &descriptorSet[frameId], 0, null );

				VkDeviceSize	stride = vulkan.GetDeviceRayTracingProperties().shaderGroupHandleSize;

				vkCmdTraceRaysNV( cmd, 
								   shaderBindingTable, RAYGEN_SHADER * stride,
								   shaderBindingTable, MISS_SHADER * stride, stride,
								   shaderBindingTable, HIT_SHADER * stride, stride,
								   VK_NULL_HANDLE, 0, 0,
								   swapchain->GetSurfaceSize().x, swapchain->GetSurfaceSize().y, 1 );
			}

			// general -> present_src
			{
				VkImageMemoryBarrier	barrier = {};
				barrier.sType				= VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
				barrier.image				= swapchain->GetCurrentImage();
				barrier.oldLayout			= VK_IMAGE_LAYOUT_GENERAL;
				barrier.newLayout			= VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
				barrier.srcAccessMask		= VK_ACCESS_SHADER_WRITE_BIT;
				barrier.dstAccessMask		= 0;
				barrier.srcQueueFamilyIndex	= VK_QUEUE_FAMILY_IGNORED;
				barrier.dstQueueFamilyIndex	= VK_QUEUE_FAMILY_IGNORED;
				barrier.subresourceRange	= { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

				vkCmdPipelineBarrier( cmd,
									  VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_NV, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
									  0, null, 0, null, 1, &barrier );
			}

			VK_CALL( vkEndCommandBuffer( cmd ));
		}


		// submit commands
		{
			VkSemaphore				signal_semaphores[] = { semaphores[1] };
			VkSemaphore				wait_semaphores[]	= { semaphores[0] };
			VkPipelineStageFlags	wait_dst_mask[]		= { VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_NV };
			STATIC_ASSERT( CountOf(wait_semaphores) == CountOf(wait_dst_mask) );

			VkSubmitInfo				submit_info = {};
			submit_info.sType					= VK_STRUCTURE_TYPE_SUBMIT_INFO;
			submit_info.commandBufferCount		= 1;
			submit_info.pCommandBuffers			= &cmd;
			submit_info.waitSemaphoreCount		= uint(CountOf(wait_semaphores));
			submit_info.pWaitSemaphores			= wait_semaphores;
			submit_info.pWaitDstStageMask		= wait_dst_mask;
			submit_info.signalSemaphoreCount	= uint(CountOf(signal_semaphores));
			submit_info.pSignalSemaphores		= signal_semaphores;

			VK_CHECK( vkQueueSubmit( cmdQueue, 1, &submit_info, fences[frameId] ));
		}

		// present
		VkResult	err = swapchain->Present( cmdQueue, {semaphores[1]} );
		switch ( err ) {
			case VK_SUCCESS :
				break;

			case VK_SUBOPTIMAL_KHR :
			case VK_ERROR_SURFACE_LOST_KHR :
			case VK_ERROR_OUT_OF_DATE_KHR :
				OnResize( swapchain->GetSurfaceSize() );
				break;

			default :
				CHECK_FATAL( !"Present failed" );
		}
	}
	return true;
}

/*
=================================================
	CreateCommandBuffers
=================================================
*/
bool RayTracingApp::CreateCommandBuffers ()
{
	VkCommandPoolCreateInfo		pool_info = {};
	pool_info.sType				= VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.queueFamilyIndex	= vulkan.GetVkQueues().front().familyIndex;
	pool_info.flags				= VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	VK_CHECK( vkCreateCommandPool( vulkan.GetVkDevice(), &pool_info, null, OUT &cmdPool ));

	VkCommandBufferAllocateInfo	info = {};
	info.sType				= VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	info.commandPool		= cmdPool;
	info.level				= VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	info.commandBufferCount	= uint(CountOf( cmdBuffers ));
	VK_CHECK( vkAllocateCommandBuffers( vulkan.GetVkDevice(), &info, OUT cmdBuffers ));

	return true;
}

/*
=================================================
	CreateSyncObjects
=================================================
*/
bool RayTracingApp::CreateSyncObjects ()
{
	VkDevice	dev = vulkan.GetVkDevice();

	VkFenceCreateInfo	fence_info	= {};
	fence_info.sType	= VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fence_info.flags	= VK_FENCE_CREATE_SIGNALED_BIT;

	for (auto& fence : fences) {
		VK_CHECK( vkCreateFence( dev, &fence_info, null, OUT &fence ));
	}
			
	VkSemaphoreCreateInfo	sem_info = {};
	sem_info.sType		= VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	sem_info.flags		= 0;

	for (auto& sem : semaphores) {
		VK_CALL( vkCreateSemaphore( dev, &sem_info, null, OUT &sem ) );
	}

	return true;
}

/*
=================================================
	CreateBottomLevelAS
=================================================
*/
bool RayTracingApp::CreateBottomLevelAS (ResourceInit &res)
{
	static const float3		vertices[] = {
		{ 0.25f, 0.25f, 0.0f },
		{ 0.75f, 0.25f, 0.0f },
		{ 0.50f, 0.75f, 0.0f }
	};
	static const uint		indices[] = {
		0, 1, 2
	};

	// create vertex buffer
	{
		VkBufferCreateInfo	info = {};
		info.sType			= VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		info.flags			= 0;
		info.size			= sizeof(vertices);
		info.usage			= VK_BUFFER_USAGE_RAY_TRACING_BIT_NV;
		info.sharingMode	= VK_SHARING_MODE_EXCLUSIVE;

		VK_CHECK( vkCreateBuffer( vulkan.GetVkDevice(), &info, null, OUT &vertexBuffer ));
		
		VkMemoryRequirements	mem_req;
		vkGetBufferMemoryRequirements( vulkan.GetVkDevice(), vertexBuffer, OUT &mem_req );
		
		VkDeviceSize	offset = AlignToLarger( res.host.totalSize, mem_req.alignment );
		res.host.totalSize		 = offset + mem_req.size;
		res.host.memTypeBits	|= mem_req.memoryTypeBits;

		res.onBind.push_back( [this, offset] (void *ptr) -> bool
		{
			memcpy( ptr + BytesU(offset), vertices, sizeof(vertices) );
			VK_CHECK( vkBindBufferMemory( vulkan.GetVkDevice(), vertexBuffer, sharedHostMemory, offset ));
			return true;
		});
	}

	// create index buffer
	{
		VkBufferCreateInfo	info = {};
		info.sType			= VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		info.flags			= 0;
		info.size			= sizeof(indices);
		info.usage			= VK_BUFFER_USAGE_RAY_TRACING_BIT_NV;
		info.sharingMode	= VK_SHARING_MODE_EXCLUSIVE;

		VK_CHECK( vkCreateBuffer( vulkan.GetVkDevice(), &info, null, OUT &indexBuffer ));
		
		VkMemoryRequirements	mem_req;
		vkGetBufferMemoryRequirements( vulkan.GetVkDevice(), indexBuffer, OUT &mem_req );
		
		VkDeviceSize	offset = AlignToLarger( res.host.totalSize, mem_req.alignment );
		res.host.totalSize		 = offset + mem_req.size;
		res.host.memTypeBits	|= mem_req.memoryTypeBits;

		res.onBind.push_back( [this, offset] (void *ptr) -> bool
		{
			memcpy( ptr + BytesU(offset), indices, sizeof(indices) );
			VK_CHECK( vkBindBufferMemory( vulkan.GetVkDevice(), indexBuffer, sharedHostMemory, offset ));
			return true;
		});
	}

	// create bottom level acceleration structure
	{
		VkGeometryNV	geometry[1] = {};
		geometry[0].sType			= VK_STRUCTURE_TYPE_GEOMETRY_NV;
		geometry[0].geometryType	= VK_GEOMETRY_TYPE_TRIANGLES_NV;
		geometry[0].flags			= VK_GEOMETRY_OPAQUE_BIT_NV;
		geometry[0].geometry.aabbs.sType	= VK_STRUCTURE_TYPE_GEOMETRY_AABB_NV;
		geometry[0].geometry.triangles.sType		= VK_STRUCTURE_TYPE_GEOMETRY_TRIANGLES_NV;
		geometry[0].geometry.triangles.vertexData	= vertexBuffer;
		geometry[0].geometry.triangles.vertexOffset	= 0;
		geometry[0].geometry.triangles.vertexCount	= uint(CountOf( vertices ));
		geometry[0].geometry.triangles.vertexStride	= sizeof(vertices[0]);
		geometry[0].geometry.triangles.vertexFormat	= VK_FORMAT_R32G32B32_SFLOAT;
		geometry[0].geometry.triangles.indexData	= indexBuffer;
		geometry[0].geometry.triangles.indexOffset	= 0;
		geometry[0].geometry.triangles.indexCount	= uint(CountOf( indices ));
		geometry[0].geometry.triangles.indexType	= VK_INDEX_TYPE_UINT32;

		VkAccelerationStructureCreateInfoNV	createinfo = {};
		createinfo.sType				= VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_NV;
		createinfo.info.sType			= VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_INFO_NV;
		createinfo.info.type			= VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_NV;
		createinfo.info.geometryCount	= uint(CountOf( geometry ));
		createinfo.info.pGeometries		= geometry;

		VK_CHECK( vkCreateAccelerationStructureNV( vulkan.GetVkDevice(), &createinfo, null, OUT &bottomLevelAS ));
		
		VkAccelerationStructureMemoryRequirementsInfoNV	mem_info = {};
		mem_info.sType					= VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_INFO_NV;
		mem_info.accelerationStructure	= bottomLevelAS;

		VkMemoryRequirements2	mem_req = {};
		vkGetAccelerationStructureMemoryRequirementsNV( vulkan.GetVkDevice(), &mem_info, OUT &mem_req );
		
		VkDeviceSize	offset = AlignToLarger( res.dev.totalSize, mem_req.memoryRequirements.alignment );
		res.dev.totalSize	 = offset + mem_req.memoryRequirements.size;
		res.dev.memTypeBits	|= mem_req.memoryRequirements.memoryTypeBits;
		
		res.onBind.push_back( [this, offset] (void *) -> bool
		{
			VkBindAccelerationStructureMemoryInfoNV	bind_info = {};
			bind_info.sType					= VK_STRUCTURE_TYPE_BIND_ACCELERATION_STRUCTURE_MEMORY_INFO_NV;
			bind_info.accelerationStructure	= bottomLevelAS;
			bind_info.memory				= sharedDevMemory;
			bind_info.memoryOffset			= offset;
			VK_CHECK( vkBindAccelerationStructureMemoryNV( vulkan.GetVkDevice(), 1, &bind_info ));

			VK_CHECK( vkGetAccelerationStructureHandleNV( vulkan.GetVkDevice(), bottomLevelAS, sizeof(bottomLevelASHandle), OUT &bottomLevelASHandle ));
			return true;
		});
		
		res.onDraw.push_back( [this, geometry] (VkCommandBuffer cmd)
		{
			VkAccelerationStructureInfoNV	info = {};
			info.sType			= VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_INFO_NV;
			info.type			= VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_NV;
			info.geometryCount	= uint(CountOf( geometry ));
			info.pGeometries	= geometry;

			vkCmdBuildAccelerationStructureNV( cmd, &info,
												VK_NULL_HANDLE, 0,					// instance
												VK_FALSE,							// update
												bottomLevelAS, VK_NULL_HANDLE,		// dst, src
												scratchBuffer, 0
											   );
		});
	}
	return true;
}

/*
=================================================
	CreateTopLevelAS
=================================================
*/
bool RayTracingApp::CreateTopLevelAS (ResourceInit &res)
{
	// create instance buffer
	{
		VkBufferCreateInfo	info = {};
		info.sType			= VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		info.flags			= 0;
		info.size			= sizeof(VkGeometryInstance);
		info.usage			= VK_BUFFER_USAGE_RAY_TRACING_BIT_NV;
		info.sharingMode	= VK_SHARING_MODE_EXCLUSIVE;

		VK_CHECK( vkCreateBuffer( vulkan.GetVkDevice(), &info, null, OUT &instanceBuffer ));
		
		VkMemoryRequirements	mem_req;
		vkGetBufferMemoryRequirements( vulkan.GetVkDevice(), instanceBuffer, OUT &mem_req );
		
		VkDeviceSize	offset = AlignToLarger( res.host.totalSize, mem_req.alignment );
		res.host.totalSize		 = offset + mem_req.size;
		res.host.memTypeBits	|= mem_req.memoryTypeBits;

		res.onBind.push_back( [this, offset] (void *ptr) -> bool
		{
			VkGeometryInstance	instance = {};
			instance.transformRow0	= {1.0f, 0.0f, 0.0f, 0.0f};
			instance.transformRow1	= {0.0f, 1.0f, 0.0f, 0.0f};
			instance.transformRow2	= {0.0f, 0.0f, 1.0f, 0.0f};
			instance.instanceId		= 0;
			instance.mask			= 0xFF;
			instance.instanceOffset	= 0;
			instance.flags			= 0;
			instance.accelerationStructureHandle = bottomLevelASHandle;

			memcpy( ptr + BytesU(offset), &instance, sizeof(instance) );

			VK_CHECK( vkBindBufferMemory( vulkan.GetVkDevice(), instanceBuffer, sharedHostMemory, offset ));
			return true;
		});
	}

	// create top level acceleration structure
	{
		VkAccelerationStructureCreateInfoNV	createinfo = {};
		createinfo.sType				= VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_NV;
		createinfo.info.sType			= VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_INFO_NV;
		createinfo.info.type			= VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_NV;
		createinfo.info.flags			= 0;
		createinfo.info.instanceCount	= 1;

		VK_CHECK( vkCreateAccelerationStructureNV( vulkan.GetVkDevice(), &createinfo, null, OUT &topLevelAS ));
		
		VkAccelerationStructureMemoryRequirementsInfoNV	mem_info = {};
		mem_info.sType					= VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_INFO_NV;
		mem_info.type					= VK_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_TYPE_OBJECT_NV;
		mem_info.accelerationStructure	= topLevelAS;

		VkMemoryRequirements2	mem_req = {};
		vkGetAccelerationStructureMemoryRequirementsNV( vulkan.GetVkDevice(), &mem_info, OUT &mem_req );

		VkDeviceSize	offset = AlignToLarger( res.dev.totalSize, mem_req.memoryRequirements.alignment );
		res.dev.totalSize	 = offset + mem_req.memoryRequirements.size;
		res.dev.memTypeBits	|= mem_req.memoryRequirements.memoryTypeBits;
		
		res.onBind.push_back( [this, offset] (void *) -> bool
		{
			VkBindAccelerationStructureMemoryInfoNV	bind_info = {};
			bind_info.sType					= VK_STRUCTURE_TYPE_BIND_ACCELERATION_STRUCTURE_MEMORY_INFO_NV;
			bind_info.accelerationStructure	= topLevelAS;
			bind_info.memory				= sharedDevMemory;
			bind_info.memoryOffset			= offset;
			VK_CHECK( vkBindAccelerationStructureMemoryNV( vulkan.GetVkDevice(), 1, &bind_info ));
			return true;
		});

		res.onDraw.push_back( [this] (VkCommandBuffer cmd)
		{
			// write-read memory barrier for 'bottomLevelAS'
			// execution barrier for 'scratchBuffer'
			VkMemoryBarrier		barrier = {};
			barrier.sType			= VK_STRUCTURE_TYPE_MEMORY_BARRIER;
			barrier.srcAccessMask	= VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_NV | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_NV;
			barrier.dstAccessMask	= VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_NV | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_NV;
			
			vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_NV, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_NV,
								  0, 1, &barrier, 0, null, 0, null );
			
			VkAccelerationStructureInfoNV	info = {};
			info.sType			= VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_INFO_NV;
			info.type			= VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_NV;
			info.flags			= 0;
			info.instanceCount	= 1;

			vkCmdBuildAccelerationStructureNV( cmd, &info,
												instanceBuffer, 0,					// instance
												VK_FALSE,							// update
												topLevelAS, VK_NULL_HANDLE,			// dst, src
												scratchBuffer, 0
											   );
		});
	}
	
	// create scratch buffer
	{
		VkBufferCreateInfo	info = {};
		info.sType			= VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		info.flags			= 0;
		info.usage			= VK_BUFFER_USAGE_RAY_TRACING_BIT_NV;
		info.sharingMode	= VK_SHARING_MODE_EXCLUSIVE;

		// calculate buffer size
		{
			VkMemoryRequirements2								mem_req2	= {};
			VkAccelerationStructureMemoryRequirementsInfoNV		as_info		= {};
			as_info.sType					= VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_INFO_NV;
			as_info.type					= VK_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_TYPE_BUILD_SCRATCH_NV;
			as_info.accelerationStructure	= topLevelAS;

			vkGetAccelerationStructureMemoryRequirementsNV( vulkan.GetVkDevice(), &as_info, OUT &mem_req2 );
			info.size = mem_req2.memoryRequirements.size;
		
			as_info.accelerationStructure	= bottomLevelAS;
			vkGetAccelerationStructureMemoryRequirementsNV( vulkan.GetVkDevice(), &as_info, OUT &mem_req2 );
			info.size = Max( info.size, mem_req2.memoryRequirements.size );
		}

		VK_CHECK( vkCreateBuffer( vulkan.GetVkDevice(), &info, null, OUT &scratchBuffer ));
		
		VkMemoryRequirements	mem_req;
		vkGetBufferMemoryRequirements( vulkan.GetVkDevice(), scratchBuffer, OUT &mem_req );
		
		VkDeviceSize	offset = AlignToLarger( res.dev.totalSize, mem_req.alignment );
		res.dev.totalSize	 = offset + mem_req.size;
		res.dev.memTypeBits	|= mem_req.memoryTypeBits;

		res.onBind.push_back( [this, offset] (void *) -> bool {
			VK_CHECK( vkBindBufferMemory( vulkan.GetVkDevice(), scratchBuffer, sharedDevMemory, offset ));
			return true;
		});
	}
	return true;
}

/*
=================================================
	CreateBindingTable
=================================================
*/
bool RayTracingApp::CreateBindingTable (ResourceInit &res)
{
	VkBufferCreateInfo	info = {};
	info.sType			= VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.flags			= 0;
	info.size			= NUM_GROUPS * vulkan.GetDeviceRayTracingProperties().shaderGroupHandleSize;
	info.usage			= VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_RAY_TRACING_BIT_NV;
	info.sharingMode	= VK_SHARING_MODE_EXCLUSIVE;

	VK_CHECK( vkCreateBuffer( vulkan.GetVkDevice(), &info, null, OUT &shaderBindingTable ));
		
	VkMemoryRequirements	mem_req;
	vkGetBufferMemoryRequirements( vulkan.GetVkDevice(), shaderBindingTable, OUT &mem_req );
		
	VkDeviceSize	offset = AlignToLarger( res.dev.totalSize, mem_req.alignment );
	res.dev.totalSize	 = offset + mem_req.size;
	res.dev.memTypeBits	|= mem_req.memoryTypeBits;

	res.onBind.push_back( [this, offset] (void *) -> bool
	{
		VK_CHECK( vkBindBufferMemory( vulkan.GetVkDevice(), shaderBindingTable, sharedDevMemory, offset ));
		return true;
	});

	res.onDraw.push_back( [this, size = info.size] (VkCommandBuffer cmd)
	{
		Array<uint8_t>	handles;  handles.resize(size);

		VK_CALL( vkGetRayTracingShaderGroupHandlesNV( vulkan.GetVkDevice(), rtPipeline, 0, NUM_GROUPS, handles.size(), OUT handles.data() ));
		
		vkCmdUpdateBuffer( cmd, shaderBindingTable, 0, handles.size(), handles.data() );
	});
	
	return true;
}

/*
=================================================
	CreateResources
=================================================
*/
bool RayTracingApp::CreateResources ()
{
	ResourceInit	res;
	res.dev.memProperty = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	res.host.memProperty = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

	CHECK_ERR( CreateBottomLevelAS( res ));
	CHECK_ERR( CreateTopLevelAS( res ));
	CHECK_ERR( CreateBindingTable( res ));

	// allocate device local memory
	{
		VkMemoryAllocateInfo	info = {};
		info.sType				= VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		info.allocationSize		= res.dev.totalSize;
		CHECK_ERR( vulkan.GetMemoryTypeIndex( res.dev.memTypeBits, res.dev.memProperty, OUT info.memoryTypeIndex ));

		VK_CHECK( vkAllocateMemory( vulkan.GetVkDevice(), &info, null, OUT &sharedDevMemory ));
	}

	// allocate host visible memory
	void* host_ptr = null;
	{
		VkMemoryAllocateInfo	info = {};
		info.sType				= VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		info.allocationSize		= res.host.totalSize;
		CHECK_ERR( vulkan.GetMemoryTypeIndex( res.host.memTypeBits, res.host.memProperty, OUT info.memoryTypeIndex ));

		VK_CHECK( vkAllocateMemory( vulkan.GetVkDevice(), &info, null, OUT &sharedHostMemory ));

		VK_CHECK( vkMapMemory( vulkan.GetVkDevice(), sharedHostMemory, 0, res.host.totalSize, 0, &host_ptr ));
	}

	// bind resources
	for (auto& bind : res.onBind) {
		CHECK_ERR( bind( host_ptr ));
	}

	// update resources
	{
		VkCommandBufferBeginInfo	begin_info = {};
		begin_info.sType	= VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		begin_info.flags	= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		VK_CALL( vkBeginCommandBuffer( cmdBuffers[0], &begin_info ));

		for (auto& cb : res.onDraw) {
			cb( cmdBuffers[0] );
		}

		VK_CALL( vkEndCommandBuffer( cmdBuffers[0] ));

		VkSubmitInfo		submit_info = {};
		submit_info.sType				= VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit_info.commandBufferCount	= 1;
		submit_info.pCommandBuffers		= &cmdBuffers[0];

		VK_CHECK( vkQueueSubmit( cmdQueue, 1, &submit_info, VK_NULL_HANDLE ));
	}
	VK_CALL( vkQueueWaitIdle( cmdQueue ));
	

	// update descriptor set
	{
		VkWriteDescriptorSetAccelerationStructureNV 	top_as = {};
		top_as.sType						= VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_NV;
		top_as.accelerationStructureCount	= 1;
		top_as.pAccelerationStructures		= &topLevelAS;

		VkWriteDescriptorSet	writes[1] = {};

		// un_RtScene
		writes[0].sType				= VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].pNext				= &top_as;
		writes[0].dstSet			= descriptorSet[0];
		writes[0].dstBinding		= 0;
		writes[0].descriptorCount	= 1;
		writes[0].descriptorType	= VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV;
		
		vkUpdateDescriptorSets( vulkan.GetVkDevice(), uint(CountOf( writes )), writes, 0, null );
		
		writes[0].dstSet = descriptorSet[1];

		vkUpdateDescriptorSets( vulkan.GetVkDevice(), uint(CountOf( writes )), writes, 0, null );
	}
	return true;
}

/*
=================================================
	CreateDescriptorSet
=================================================
*/
bool RayTracingApp::CreateDescriptorSet ()
{
	// create layout
	{
		VkDescriptorSetLayoutBinding		binding[2] = {};
		binding[0].binding			= 0;
		binding[0].descriptorType	= VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV;
		binding[0].descriptorCount	= 1;
		binding[0].stageFlags		= VK_SHADER_STAGE_RAYGEN_BIT_NV;

		binding[1].binding			= 1;
		binding[1].descriptorType	= VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		binding[1].descriptorCount	= 1;
		binding[1].stageFlags		= VK_SHADER_STAGE_RAYGEN_BIT_NV;

		VkDescriptorSetLayoutCreateInfo		info = {};
		info.sType			= VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		info.bindingCount	= uint(CountOf( binding ));
		info.pBindings		= binding;

		VK_CHECK( vkCreateDescriptorSetLayout( vulkan.GetVkDevice(), &info, null, OUT &dsLayout ));
	}

	// create pool
	{
		const VkDescriptorPoolSize		sizes[] = {
			{ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV, 100 },
			{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 100 }
		};

		VkDescriptorPoolCreateInfo		info = {};
		info.sType			= VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		info.maxSets		= 100;
		info.poolSizeCount	= uint(CountOf( sizes ));
		info.pPoolSizes		= sizes;

		VK_CHECK( vkCreateDescriptorPool( vulkan.GetVkDevice(), &info, null, OUT &descriptorPool ));
	}

	// allocate descriptor set
	{
		VkDescriptorSetAllocateInfo		info = {};
		info.sType				= VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		info.descriptorPool		= descriptorPool;
		info.descriptorSetCount	= 1;
		info.pSetLayouts		= &dsLayout;

		VK_CHECK( vkAllocateDescriptorSets( vulkan.GetVkDevice(), &info, OUT &descriptorSet[0] ));
		VK_CHECK( vkAllocateDescriptorSets( vulkan.GetVkDevice(), &info, OUT &descriptorSet[1] ));
	}
	return true;
}

/*
=================================================
	CreateRayTracingPipeline
=================================================
*/
bool RayTracingApp::CreateRayTracingPipeline ()
{
	static const char	rt_shader[] = R"#(
#extension GL_NV_ray_tracing : require
#define PAYLOAD_LOC 0
)#";

	// create ray generation shader
	{
		static const char	raygen_shader_source[] = R"#(
layout(binding = 0) uniform accelerationStructureNV  un_RtScene;
layout(binding = 1, rgba8) writeonly restrict uniform image2D  un_Output;
layout(location = PAYLOAD_LOC) rayPayloadNV vec4  payload;

void main ()
{
	const vec2 uv = vec2(gl_LaunchIDNV.xy) / vec2(gl_LaunchSizeNV.xy - 1);

	const vec3 origin = vec3(uv.x, 1.0f - uv.y, -1.0f);
	const vec3 direction = vec3(0.0f, 0.0f, 1.0f);

	traceNV( /*topLevel*/un_RtScene, /*rayFlags*/gl_RayFlagsNoneNV, /*cullMask*/0xFF,
			  /*sbtRecordOffset*/0, /*sbtRecordStride*/0, /*missIndex*/0,
			  /*origin*/origin, /*Tmin*/0.0f,
			  /*direction*/direction, /*Tmax*/10.0f,
			  /*payload*/PAYLOAD_LOC );

	imageStore( un_Output, ivec2(gl_LaunchIDNV), payload );
}
)#";
		CHECK_ERR( spvCompiler.Compile( OUT rayGenShader, vulkan, {rt_shader, raygen_shader_source}, "main", EShLangRayGenNV ));
	}

	// create ray miss shader
	{
		static const char	raymiss_shader_source[] = R"#(
layout(location = PAYLOAD_LOC) rayPayloadInNV vec4  payload;

void main ()
{
	payload = vec4( 0.412f, 0.796f, 1.0f, 1.0f );
}
)#";
		CHECK_ERR( spvCompiler.Compile( OUT rayMissShader, vulkan, {rt_shader, raymiss_shader_source}, "main", EShLangMissNV ));
	}

	// create ray closest hit shader
	{
		static const char	closesthit_shader_source[] = R"#(
layout(location = PAYLOAD_LOC) rayPayloadInNV vec4  payload;
hitAttributeNV vec2  HitAttribs;

void main ()
{
	const vec3 barycentrics = vec3(1.0f - HitAttribs.x - HitAttribs.y, HitAttribs.x, HitAttribs.y);
	payload = vec4(barycentrics, 1.0);
}
)#";
		CHECK_ERR( spvCompiler.Compile( OUT rayClosestHitShader, vulkan, {rt_shader, closesthit_shader_source}, "main", EShLangClosestHitNV ));
	}

	// create pipeline layout
	{
		VkPipelineLayoutCreateInfo	info = {};
		info.sType					= VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		info.setLayoutCount			= 1;
		info.pSetLayouts			= &dsLayout;
		info.pushConstantRangeCount	= 0;
		info.pPushConstantRanges	= null;

		VK_CHECK( vkCreatePipelineLayout( vulkan.GetVkDevice(), &info, null, OUT &pplnLayout ));
	}
	

	VkPipelineShaderStageCreateInfo		stages [NUM_GROUPS] = {};

	stages[RAYGEN_SHADER].sType		= VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[RAYGEN_SHADER].stage		= VK_SHADER_STAGE_RAYGEN_BIT_NV;
	stages[RAYGEN_SHADER].module	= rayGenShader;
	stages[RAYGEN_SHADER].pName		= "main";

	stages[MISS_SHADER].sType		= VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[MISS_SHADER].stage		= VK_SHADER_STAGE_MISS_BIT_NV;
	stages[MISS_SHADER].module		= rayMissShader;
	stages[MISS_SHADER].pName		= "main";

	stages[HIT_SHADER].sType		= VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[HIT_SHADER].stage		= VK_SHADER_STAGE_CLOSEST_HIT_BIT_NV;
	stages[HIT_SHADER].module		= rayClosestHitShader;
	stages[HIT_SHADER].pName		= "main";


	VkRayTracingShaderGroupCreateInfoNV	shader_groups [NUM_GROUPS] = {};

	shader_groups[RAYGEN_SHADER].sType				= VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_NV;
	shader_groups[RAYGEN_SHADER].type				= VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_NV;
	shader_groups[RAYGEN_SHADER].generalShader		= RAYGEN_SHADER;
	shader_groups[RAYGEN_SHADER].closestHitShader	= VK_SHADER_UNUSED_NV;
	shader_groups[RAYGEN_SHADER].anyHitShader		= VK_SHADER_UNUSED_NV;
	shader_groups[RAYGEN_SHADER].intersectionShader	= VK_SHADER_UNUSED_NV;
	
	shader_groups[HIT_SHADER].sType					= VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_NV;
	shader_groups[HIT_SHADER].type					= VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_NV;
	shader_groups[HIT_SHADER].generalShader			= VK_SHADER_UNUSED_NV;
	shader_groups[HIT_SHADER].closestHitShader		= HIT_SHADER;
	shader_groups[HIT_SHADER].anyHitShader			= VK_SHADER_UNUSED_NV;
	shader_groups[HIT_SHADER].intersectionShader	= VK_SHADER_UNUSED_NV;
	
	shader_groups[MISS_SHADER].sType				= VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_NV;
	shader_groups[MISS_SHADER].type					= VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_NV;
	shader_groups[MISS_SHADER].generalShader		= MISS_SHADER;
	shader_groups[MISS_SHADER].closestHitShader		= VK_SHADER_UNUSED_NV;
	shader_groups[MISS_SHADER].anyHitShader			= VK_SHADER_UNUSED_NV;
	shader_groups[MISS_SHADER].intersectionShader	= VK_SHADER_UNUSED_NV;


	// create pipeline
	VkRayTracingPipelineCreateInfoNV 	info = {};
	info.sType				= VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_NV;
	info.flags				= 0;
	info.stageCount			= uint(CountOf( stages ));
	info.pStages			= stages;
	info.pGroups			= shader_groups;
	info.groupCount			= uint(CountOf( shader_groups ));
	info.maxRecursionDepth	= 0;
	info.layout				= pplnLayout;

	VK_CHECK( vkCreateRayTracingPipelinesNV( vulkan.GetVkDevice(), VK_NULL_HANDLE, 1, &info, null, OUT &rtPipeline ));
	return true;
}
}	// anonymous namespace

/*
=================================================
	RayTracing_Sample1
=================================================
*/
extern void RayTracing_Sample1 ()
{
	RayTracingApp	app;
	
	if ( app.Initialize() )
	{
		app.Run();
		app.Destroy();
	}
}
