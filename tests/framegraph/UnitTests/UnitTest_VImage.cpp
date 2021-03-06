// Copyright (c) 2018-2019,  Zhirnov Andrey. For more information see 'LICENSE'

#include "VLocalImage.h"
#include "VBarrierManager.h"
#include "framegraph/Public/FrameGraph.h"
#include "UnitTest_Common.h"
#include "DummyTask.h"


namespace FG
{
	class VImageUnitTest
	{
	public:
		using Barrier = VLocalImage::ImageAccess;

		static bool Create (VImage &img, const ImageDesc &desc)
		{
			img._desc	= desc;
			img._desc.Validate();

			img._defaultLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			return true;
		}

		static ArrayView<Barrier>  GetRWBarriers (const VLocalImage *img) {
			return img->_accessForReadWrite;
		}
	};

	using ImageState	= VLocalImage::ImageState;
	using ImageRange	= VLocalImage::ImageRange;
}	// FG


static void CheckLayers (const VImageUnitTest::Barrier &barrier, uint arrayLayers,
						 uint baseMipLevel, uint levelCount, uint baseArrayLayer, uint layerCount)
{
	uint	base_mip_level		= (barrier.range.begin / arrayLayers);
	uint	level_count			= Max( 1u, (barrier.range.end - barrier.range.begin) / arrayLayers );
	uint	base_array_layer	= (barrier.range.begin % arrayLayers);
	uint	layer_count			= Max( 1u, (barrier.range.end - barrier.range.begin) % arrayLayers );

	TEST( base_mip_level == baseMipLevel );
	TEST( level_count == levelCount );
	TEST( base_array_layer == baseArrayLayer );
	TEST( layer_count == layerCount );
}


static void VImage_Test1 ()
{
	VBarrierManager		barrier_mngr;
	
	const auto			tasks		= GenDummyTasks( 30 );
	auto				task_iter	= tasks.begin();

	VImage				global_image;
	VLocalImage			local_image;
	VLocalImage const*	img			= &local_image;

	TEST( VImageUnitTest::Create( global_image,
								  ImageDesc{ EImage::Tex2D, uint3(64, 64, 0), EPixelFormat::RGBA8_UNorm,
											 EImageUsage::ColorAttachment | EImageUsage::Transfer | EImageUsage::Storage | EImageUsage::Sampled,
											 0_layer, 11_mipmap } ));

	TEST( local_image.Create( &global_image ));

	
	// pass 1
	{
		img->AddPendingState(ImageState{ EResourceState::TransferDst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
								ImageRange{ 0_layer, 1, 0_mipmap, 1 },
								VK_IMAGE_ASPECT_COLOR_BIT, (task_iter++)->get() });

		img->CommitBarrier( barrier_mngr, null );

		auto	barriers = VImageUnitTest::GetRWBarriers( img );

		TEST( barriers.size() == 2 );

		TEST( barriers[0].range.begin == 0 );
		TEST( barriers[0].range.end == 1 );
		TEST( barriers[0].stages == VK_PIPELINE_STAGE_TRANSFER_BIT );
		TEST( barriers[0].access == VK_ACCESS_TRANSFER_WRITE_BIT );
		TEST( barriers[0].isReadable == false );
		TEST( barriers[0].isWritable == true );
		TEST( barriers[0].layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );
		TEST( barriers[0].index == ExeOrderIndex(1) );

		CheckLayers( barriers[0], img->ArrayLayers(), 0, 1, 0, img->ArrayLayers() );
		
		TEST( barriers[1].range.begin == 1 );
		TEST( barriers[1].range.end == 7 );
		TEST( barriers[1].stages == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT );
		TEST( barriers[1].access == 0 );
		TEST( barriers[1].isReadable == false );
		TEST( barriers[1].isWritable == false );
		TEST( barriers[1].layout == VK_IMAGE_LAYOUT_UNDEFINED );
		TEST( barriers[1].index == ExeOrderIndex::Initial );

		CheckLayers( barriers[1], img->ArrayLayers(), 1, img->MipmapLevels()-1, 0, img->ArrayLayers() );
	}
	
	local_image.ResetState( ExeOrderIndex::Final, barrier_mngr, null );

	local_image.Destroy();
	//global_image.Destroy();
}


static void VImage_Test2 ()
{
	VBarrierManager		barrier_mngr;
	
	const auto			tasks		= GenDummyTasks( 30 );
	auto				task_iter	= tasks.begin();
	
	VImage				global_image;
	VLocalImage			local_image;
	VLocalImage const*	img			= &local_image;

	TEST( VImageUnitTest::Create( global_image,
								  ImageDesc{ EImage::Tex2DArray, uint3(64, 64, 0), EPixelFormat::RGBA8_UNorm,
											 EImageUsage::ColorAttachment | EImageUsage::Transfer | EImageUsage::Storage | EImageUsage::Sampled,
											 8_layer, 11_mipmap } ));

	TEST( local_image.Create( &global_image ));

	// pass 1
	{
		img->AddPendingState(ImageState{ EResourceState::TransferDst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
								ImageRange{ 0_layer, 2, 0_mipmap, img->MipmapLevels() },
								VK_IMAGE_ASPECT_COLOR_BIT, (task_iter++)->get() });

		img->CommitBarrier( barrier_mngr, null );

		auto	barriers = VImageUnitTest::GetRWBarriers( img );
		
		TEST( barriers.size() == 14 );

		TEST( barriers[0].range.begin == 0 );
		TEST( barriers[0].range.end == 2 );
		TEST( barriers[0].stages == VK_PIPELINE_STAGE_TRANSFER_BIT );
		TEST( barriers[0].access == VK_ACCESS_TRANSFER_WRITE_BIT );
		TEST( barriers[0].isReadable == false );
		TEST( barriers[0].isWritable == true );
		TEST( barriers[0].layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );
		TEST( barriers[0].index == ExeOrderIndex(1) );
		
		CheckLayers( barriers[0], img->ArrayLayers(), 0, 1, 0, 2 );

		TEST( barriers[1].range.begin == 2 );
		TEST( barriers[1].range.end == 8 );
		TEST( barriers[1].stages == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT );
		TEST( barriers[1].access == 0 );
		TEST( barriers[1].isReadable == false );
		TEST( barriers[1].isWritable == false );
		TEST( barriers[1].layout == VK_IMAGE_LAYOUT_UNDEFINED );
		TEST( barriers[1].index == ExeOrderIndex::Initial );

		CheckLayers( barriers[1], img->ArrayLayers(), 0, 1, 2, img->ArrayLayers()-2 );
		
		CheckLayers( barriers[2], img->ArrayLayers(), 1, 1, 0, 2 );
		CheckLayers( barriers[3], img->ArrayLayers(), 1, 1, 2, img->ArrayLayers()-2 );
	}
	
	local_image.ResetState( ExeOrderIndex::Final, barrier_mngr, null );

	local_image.Destroy();
	//global_image.Destroy();
}


extern void UnitTest_VImage ()
{
	VImage_Test1();
	VImage_Test2();
	FG_LOGI( "UnitTest_VImage - passed" );
}
