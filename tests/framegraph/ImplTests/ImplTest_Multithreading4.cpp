// Copyright (c) 2018-2019,  Zhirnov Andrey. For more information see 'LICENSE'

#include "../FGApp.h"
#include "stl/ThreadSafe/Barrier.h"
#include <thread>

namespace FG
{
	static constexpr uint		max_count		= 1000;
	static GPipelineID			gpipeline;
	static CPipelineID			cpipeline;
	static Barrier				sync			{4};
	static ImageID				images[4]		= {};
	static CommandBuffer		cmdBuffers [4]	= {};
	static CommandBuffer		perFrame[2]		= {};
	static const EQueueUsage	queueUsage		= EQueueUsage::All;


	static bool RenderThread1 (const FrameGraph &fg)
	{
		const uint2	view_size	= {800, 600};

		images[0] = fg->CreateImage( ImageDesc{ EImage::Tex2D, uint3{view_size.x, view_size.y, 1}, EPixelFormat::RGBA8_UNorm,
												EImageUsage::ColorAttachment | EImageUsage::TransferSrc }.SetQueues( queueUsage ),
									 Default, "RenderTarget1" );
		
		// (0) wait until all shared resources has been initialized
		sync.wait();

		for (uint i = 0; i < max_count; ++i)
		{
			fg->Wait({ perFrame[i&1] });

			CommandBuffer cmd = fg->Begin( CommandBufferDesc{ EQueueType::Graphics });
			CHECK_ERR( cmd );
			
			perFrame[i&1] = cmd;
			cmdBuffers[0] = cmd;

			// add dependency from previous frame because this commands shares same data and used in different queues
			cmd->AddDependency( cmdBuffers[3] );

			// (1) wake up all render threads
			sync.wait();

			LogicalPassID	render_pass	= cmd->CreateRenderPass( RenderPassDesc( view_size )
												.AddTarget( RenderTargetID(0), images[0], RGBA32f(0.0f), EAttachmentStoreOp::Store )
												.AddViewport( view_size ) );
		
			cmd->AddTask( render_pass, DrawVertices().Draw( 3 ).SetPipeline( gpipeline ).SetTopology( EPrimitive::TriangleList ));

			Task	t_draw	= cmd->AddTask( SubmitRenderPass{ render_pass });
			FG_UNUSED( t_draw );

			CHECK_ERR( fg->Execute( cmd ));
			
			// (2) wait until all threads complete command buffer recording
			sync.wait();

			CHECK_ERR( fg->Flush() );
		}
		
		fg->ReleaseResource( images[0] );
		return true;
	}


	static bool RenderThread2 (const FrameGraph &fg)
	{
		const uint2	view_size	= {1024, 1024};
		const uint2	local_size	= { 16, 16 };
		
		images[1] = fg->CreateImage( ImageDesc{ EImage::Tex2D, uint3{view_size.x, view_size.y, 1}, EPixelFormat::RGBA8_UNorm,
												EImageUsage::Storage | EImageUsage::TransferSrc }.SetQueues( queueUsage ),
									 Default, "RenderTarget2" );
		
		PipelineResources	resources;
		CHECK_ERR( fg->InitPipelineResources( cpipeline, DescriptorSetID("0"), OUT resources ));
		resources.BindImage( UniformID("un_OutImage"), images[1] );
		
		// (0) wait until all shared resources has been initialized
		sync.wait();

		for (uint i = 0; i < max_count; ++i)
		{
			CommandBuffer cmd = fg->Begin( CommandBufferDesc{ EQueueType::AsyncCompute });
			CHECK_ERR( cmd );
			
			cmdBuffers[1] = cmd;

			// (1) wait for first command buffer
			sync.wait();
			cmd->AddDependency( cmdBuffers[0] );
			
			Task	t_comp	= cmd->AddTask( DispatchCompute().SetPipeline( cpipeline ).AddResources( DescriptorSetID("0"), &resources )
														.SetLocalSize( local_size ).Dispatch( view_size / local_size ));
			FG_UNUSED( t_comp );

			CHECK_ERR( fg->Execute( cmd ));
			
			// (2) notify that thread has already finished recording the command buffer
			sync.wait();
		}

		fg->ReleaseResource( images[1] );
		return true;
	}


	static bool RenderThread3 (const FrameGraph &fg)
	{
		const uint2	view_size	= {500, 1700};
		
		images[2] = fg->CreateImage( ImageDesc{ EImage::Tex2D, uint3{view_size.x, view_size.y, 1}, EPixelFormat::RGBA16_UNorm,
												EImageUsage::ColorAttachment | EImageUsage::TransferSrc }.SetQueues( queueUsage ),
									 Default, "RenderTarget3" );
		
		// (0) wait until all shared resources has been initialized
		sync.wait();

		for (uint i = 0; i < max_count; ++i)
		{
			// (1) wait for second command buffer
			sync.wait();

			CommandBuffer cmd = fg->Begin( CommandBufferDesc{ EQueueType::Graphics });
			CHECK_ERR( cmd );
			
			cmdBuffers[2] = cmd;
			cmd->AddDependency( cmdBuffers[1] );

			LogicalPassID	render_pass	= cmd->CreateRenderPass( RenderPassDesc( view_size )
												.AddTarget( RenderTargetID(0), images[2], RGBA32f(0.0f), EAttachmentStoreOp::Store )
												.AddViewport( view_size ) );
		
			cmd->AddTask( render_pass, DrawVertices().Draw( 3 ).SetPipeline( gpipeline ).SetTopology( EPrimitive::TriangleList ));

			Task	t_draw	= cmd->AddTask( SubmitRenderPass{ render_pass });
			FG_UNUSED( t_draw );

			CHECK_ERR( fg->Execute( cmd ));
			
			// (2) notify that thread has already finished recording the command buffer
			sync.wait();
		}

		fg->ReleaseResource( images[2] );
		return true;
	}


	static bool RenderThread4 (const FrameGraph &fg)
	{
		const uint2	view_size	= {1024, 1024};
		
		images[3] = fg->CreateImage( ImageDesc{ EImage::Tex2D, uint3{view_size.x, view_size.y, 1}, EPixelFormat::RGBA8_UNorm,
											    EImageUsage::TransferDst }.SetQueues( queueUsage ),
									 Default, "RenderTarget4" );
		
		// (0) wait until all shared resources has been initialized
		sync.wait();

		for (uint i = 0; i < max_count; ++i)
		{
			CommandBuffer cmd = fg->Begin( CommandBufferDesc{ EQueueType::AsyncTransfer });
			CHECK_ERR( cmd );

			// (1) wait for afirst and second command buffers
			sync.wait();
			
			cmdBuffers[3] = cmd;

			cmd->AddDependency( cmdBuffers[0] );
			cmd->AddDependency( cmdBuffers[1] );

			Task	t_copy1 = cmd->AddTask( CopyImage{}.From( images[0] ).To( images[3] ).AddRegion( {}, int2{16, 16}, {}, int2{0,0}, uint2{256, 256} ));
			Task	t_copy2 = cmd->AddTask( CopyImage{}.From( images[1] ).To( images[3] ).AddRegion( {}, int2{256, 256}, {}, int2{256, 256}, uint2{256, 256} ));
			FG_UNUSED( t_copy1 and t_copy2 );

			CHECK_ERR( fg->Execute( cmd ));
			
			// (2) notify that thread has already finished recording the command buffer
			sync.wait();
		}

		fg->ReleaseResource( images[3] );
		return true;
	}


	bool FGApp::ImplTest_Multithreading4 ()
	{
		GraphicsPipelineDesc	gppln;
		gppln.AddShader( EShader::Vertex, EShaderLangFormat::VKSL_100, "main", R"#(
#pragma shader_stage(vertex)
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

out vec3	v_Color;

const vec2	g_Positions[3] = vec2[](
	vec2(0.0, -0.5),
	vec2(0.5, 0.5),
	vec2(-0.5, 0.5)
);

const vec3	g_Colors[3] = vec3[](
	vec3(1.0, 0.0, 0.0),
	vec3(0.0, 1.0, 0.0),
	vec3(0.0, 0.0, 1.0)
);

void main() {
	gl_Position	= vec4( g_Positions[gl_VertexIndex], 0.0, 1.0 );
	v_Color		= g_Colors[gl_VertexIndex];
}
)#" );
		gppln.AddShader( EShader::Fragment, EShaderLangFormat::VKSL_100, "main", R"#(
#pragma shader_stage(fragment)
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

in  vec3	v_Color;
out vec4	out_Color;

void main() {
	out_Color = vec4(v_Color, 1.0);
}
)#" );
		gpipeline = _frameGraph->CreatePipeline( gppln );
		
		ComputePipelineDesc	cppln;
		cppln.AddShader( EShaderLangFormat::VKSL_100, "main", R"#(
#pragma shader_stage(compute)
#extension GL_ARB_shading_language_420pack : enable

layout (local_size_x_id = 0, local_size_y_id = 1, local_size_z = 1) in;
layout (local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(rgba8) writeonly uniform image2D  un_OutImage;

void main ()
{
	vec4 fragColor = vec4(float(gl_LocalInvocationID.x) / float(gl_WorkGroupSize.x),
						  float(gl_LocalInvocationID.y) / float(gl_WorkGroupSize.y),
						  1.0, 0.0);

	imageStore( un_OutImage, ivec2(gl_GlobalInvocationID.xy), fragColor );
}
)#" );
		cpipeline = _frameGraph->CreatePipeline( cppln );

		bool			thread1_result, thread2_result, thread3_result, thread4_result;

		std::thread		thread1( [this, &thread1_result]() { thread1_result = RenderThread1( _frameGraph ); });
		std::thread		thread2( [this, &thread2_result]() { thread2_result = RenderThread2( _frameGraph ); });
		std::thread		thread3( [this, &thread3_result]() { thread3_result = RenderThread3( _frameGraph ); });
		std::thread		thread4( [this, &thread4_result]() { thread4_result = RenderThread4( _frameGraph ); });

		thread1.join();
		thread2.join();
		thread3.join();
		thread4.join();

		CHECK_ERR( _frameGraph->WaitIdle() );
		CHECK_ERR( thread1_result and thread2_result and thread3_result and thread4_result );

		for (auto& cmd : cmdBuffers) { cmd = null; }
		for (auto& cmd : perFrame) { cmd = null; }

		DeleteResources( gpipeline, cpipeline );

		FG_LOGI( TEST_NAME << " - passed" );
		return true;
	}

}	// FG
