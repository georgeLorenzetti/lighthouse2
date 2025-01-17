﻿/* rendercore.cpp - Copyright 2019 Utrecht University

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

	   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#include "core_settings.h"

namespace lh2core {

// forward declaration of cuda code
const surfaceReference* renderTargetRef();
void finalizeRender( const float4* accumulator, const int w, const int h, const int spp, const float brightness, const float contrast );
void shade( const int pathCount, float4* accumulator, const uint stride,
	float4* pathStates, const float4* hits, float4* connections,
	const uint R0, const uint* blueNoise, const int pass,
	const int probePixelIdx, const int pathLength, const int w, const int h, const float spreadAngle,
	const float3 p1, const float3 p2, const float3 p3, const float3 pos );
void InitCountersForExtend( int pathCount );
void InitCountersSubsequent();

// setters / getters
void SetInstanceDescriptors( CoreInstanceDesc* p );
void SetMaterialList( CoreMaterial* p );
void SetAreaLights( CoreLightTri* p );
void SetPointLights( CorePointLight* p );
void SetSpotLights( CoreSpotLight* p );
void SetDirectionalLights( CoreDirectionalLight* p );
void SetLightCounts( int area, int point, int spot, int directional );
void SetARGB32Pixels( uint* p );
void SetARGB128Pixels( float4* p );
void SetNRM32Pixels( uint* p );
void SetSkyPixels( float3* p );
void SetSkySize( int w, int h );
void SetPathStates( PathState* p );
void SetDebugData( float4* p );
void SetGeometryEpsilon( float e );
void SetClampValue( float c );
void SetCounters( Counters* p );

} // namespace lh2core

using namespace lh2core;

Context RenderCore::context = 0;
Program RenderCore::optixRaygen;

//  +-----------------------------------------------------------------------------+
//  |  RenderCore::SetProbePos                                                    |
//  |  Set the pixel for which the triid will be captured.                  LH2'19|
//  +-----------------------------------------------------------------------------+
void RenderCore::SetProbePos( int2 pos )
{
	probePos = pos; // triangle id for this pixel will be stored in coreStats
}

void rtUsageCallback( int a, const char* b, const char* c, void* d )
{
	printf( "%s %s", b, c );
}

//  +-----------------------------------------------------------------------------+
//  |  RenderCore::Init                                                           |
//  |  Initialization.                                                      LH2'19|
//  +-----------------------------------------------------------------------------+
void RenderCore::Init()
{
	// select the fastest device
	uint device = CUDATools::FastestDevice();
	cudaSetDevice( device );
	cudaDeviceProp properties;
	cudaGetDeviceProperties( &properties, device );
	coreStats.SMcount = SMcount = properties.multiProcessorCount;
	coreStats.ccMajor = properties.major;
	coreStats.ccMinor = properties.minor;
	computeCapability = coreStats.ccMajor * 10 + coreStats.ccMinor;
	coreStats.VRAM = (uint)(properties.totalGlobalMem >> 20);
	coreStats.deviceName = new char[strlen( properties.name ) + 1];
	memcpy( coreStats.deviceName, properties.name, strlen( properties.name ) + 1 );
	printf( "running on GPU: %s (%i SMs, %iGB VRAM)\n", coreStats.deviceName, coreStats.SMcount, (int)(coreStats.VRAM >> 10) );
	// setup OptiX context
	context = Context::create();
	context->setRayTypeCount( 2 );
	context->setEntryPointCount( 1 );
	context->setMaxTraceDepth( 1 );
	context->setMaxCallableProgramDepth( 1 );
	context->setPrintEnabled( 0 );
	context->setExceptionEnabled( RT_EXCEPTION_ALL, false );
	// compile cuda code to ptx and obtain programs
	string ptx;
	if (NeedsRecompile( "../../lib/RenderCore_OptixRTX_B/optix/", ".optix.turing.cu.ptx", ".optix.cu", "../../rendersystem/common_settings.h", "../core_settings.h" ))
	{
		CUDATools::compileToPTX( ptx, TextFileRead( "../../lib/RenderCore_OptixRTX_B/optix/.optix.cu" ).c_str(), "../../lib/RenderCore_OptixRTX_B/optix", computeCapability, 6 );
		if (computeCapability / 10 == 7) TextFileWrite( ptx, "../../lib/RenderCore_OptixRTX_B/optix/.optix.turing.cu.ptx" );
		else if (computeCapability / 10 == 6) TextFileWrite( ptx, "../../lib/RenderCore_OptixRTX_B/optix/.optix.pascal.cu.ptx" );
		else if (computeCapability / 10 == 5) TextFileWrite( ptx, "../../lib/RenderCore_OptixRTX_B/optix/.optix.maxwell.cu.ptx" );
		printf( "recompiled .optix.cu.\n" );
	}
	else
	{
		FILE* f;
		if (coreStats.ccMajor == 7) fopen_s( &f, "../../lib/RenderCore_OptixRTX_B/optix/.optix.turing.cu.ptx", "rb" );
		else if (coreStats.ccMajor == 6) fopen_s( &f, "../../lib/RenderCore_OptixRTX_B/optix/.optix.pascal.cu.ptx", "rb" );
		else if (coreStats.ccMajor == 5) fopen_s( &f, "../../lib/RenderCore_OptixRTX_B/optix/.optix.maxwell.cu.ptx", "rb" );
		int len;
		fread( &len, 1, 4, f );
		char* t = new char[len];
		fread( t, 1, len, f );
		fclose( f );
		ptx = string( t );
		delete t;
	}
	context->setRayGenerationProgram( 0, context->createProgramFromPTXString( ptx, "generate" ) );
#ifdef _DEBUG
	// check for exceptions only in debug
	context->setExceptionProgram( 0, context->createProgramFromPTXString( ptx, "exception" ) );
#endif
	// material
	dummyMaterial = context->createMaterial();
	dummyMaterial->setClosestHitProgram( 0u, context->createProgramFromPTXString( ptx, "closesthit" ) );
	dummyMaterial->setAnyHitProgram( 1u, context->createProgramFromPTXString( ptx, "any_hit_shadow" ) );
	// prepare the top-level 'model' node; instances will be added to this.
	topLevelGroup = context->createGroup();
	topLevelGroup->setAcceleration( context->createAcceleration( "Trbvh" ) );
	context["bvhRoot"]->set( topLevelGroup );
	// prepare performance counters
	performanceCounters = context->createBuffer( RT_BUFFER_INPUT_OUTPUT, RT_FORMAT_UNSIGNED_INT, 64 );
	RenderCore::context["performanceCounters"]->setBuffer( performanceCounters );
	// prepare counters for persistent threads
	counterBuffer = new CoreBuffer<Counters>( 1, ON_HOST | ON_DEVICE );
	SetCounters( counterBuffer->DevPtr() );
	// render settings
	SetClampValue( 10.0f );
	// prepare the bluenoise data
	const uchar* data8 = (const uchar*)sob256_64; // tables are 8 bit per entry
	uint* data32 = new uint[65536 * 5]; // we want a full uint per entry
	for (int i = 0; i < 65536; i++) data32[i] = data8[i]; // convert
	data8 = (uchar*)scr256_64;
	for (int i = 0; i < (128 * 128 * 8); i++) data32[i + 65536] = data8[i];
	data8 = (uchar*)rnk256_64;
	for (int i = 0; i < (128 * 128 * 8); i++) data32[i + 3 * 65536] = data8[i];
	blueNoise = new InteropBuffer<uint>( 65536 * 5, ON_DEVICE, RT_BUFFER_INPUT, RT_FORMAT_UNSIGNED_INT, "blueNoise", data32 );
	delete data32;
	// allow CoreMeshes to access the core
	CoreMesh::renderCore = this;
	CoreMesh::attribProgram = context->createProgramFromPTXString( ptx, "triangle_attributes" );
	// prepare timing events
	for( int i = 0; i < MAXPATHLENGTH; i++ )
	{
		cudaEventCreate( &shadeStart[i] );
		cudaEventCreate( &shadeEnd[i] );
	}
}

//  +-----------------------------------------------------------------------------+
//  |  RenderCore::SetTarget                                                      |
//  |  Set the OpenGL texture that serves as the render target.             LH2'19|
//  +-----------------------------------------------------------------------------+
void RenderCore::SetTarget( GLTexture* target, const uint spp )
{
	// synchronize OpenGL viewport
	scrwidth = target->width;
	scrheight = target->height;
	scrspp = spp;
	renderTarget.SetTexture( target );
	bool firstFrame = (maxPixels == 0);
	// notify CUDA about the texture
	renderTarget.LinkToSurface( renderTargetRef() );
	// see if we need to reallocate our buffers
	bool reallocate = false;
	if (scrwidth * scrheight > maxPixels || spp != currentSPP)
	{
		maxPixels = scrwidth * scrheight;
		maxPixels += maxPixels >> 4; // reserve a bit extra to prevent frequent reallocs
		currentSPP = spp;
		reallocate = true;
	}
	// notify OptiX about the new screen size
	context["scrsize"]->set3iv( (const int*)&make_int3( scrwidth, scrheight, scrspp ) );
	if (reallocate)
	{
		// reallocate buffers
		delete connectionBuffer;
		delete accumulator;
		delete hitBuffer;
		delete pathStateBuffer;
		connectionBuffer = new InteropBuffer<float4>( maxPixels * scrspp * 3 * MAXPATHLENGTH, ON_DEVICE, RT_BUFFER_INPUT, RT_FORMAT_FLOAT4, "connectData" );
		accumulator = new InteropBuffer<float4>( maxPixels * 2 /* to split direct / indirect */, ON_DEVICE, RT_BUFFER_INPUT_OUTPUT, RT_FORMAT_FLOAT4, "accumulator" );
		hitBuffer = new InteropBuffer<float4>( maxPixels * scrspp, ON_DEVICE, RT_BUFFER_OUTPUT, RT_FORMAT_FLOAT4, "hitData" );
		pathStateBuffer = new InteropBuffer<float4>( maxPixels * scrspp * 3, ON_DEVICE, RT_BUFFER_INPUT_OUTPUT, RT_FORMAT_FLOAT4, "pathStates" );
		printf( "buffers resized for %i pixels @ %i samples.\n", maxPixels, scrspp );
	}
	// clear the accumulator
	accumulator->Clear( ON_DEVICE );
	samplesTaken = 0;
}

//  +-----------------------------------------------------------------------------+
//  |  RenderCore::SetGeometry                                                    |
//  |  Set the geometry data for a model.                                   LH2'19|
//  +-----------------------------------------------------------------------------+
void RenderCore::SetGeometry( const int meshIdx, const float4* vertexData, const int vertexCount, const int triangleCount, const CoreTri* triangles, const uint* alphaFlags )
{
	// Note: for first-time setup, meshes are expected to be passed in sequential order.
	// This will result in new CoreMesh pointers being pushed into the meshes vector.
	// Subsequent mesh changes will be applied to existing CoreMeshes. This is deliberately
	// minimalistic; RenderSystem is responsible for a proper (fault-tolerant) interface.
	if (meshIdx >= meshes.size()) meshes.push_back( new CoreMesh() );
	meshes[meshIdx]->SetGeometry( vertexData, vertexCount, triangleCount, triangles, alphaFlags );
}

//  +-----------------------------------------------------------------------------+
//  |  RenderCore::SetInstance                                                    |
//  |  Set instance details.                                                LH2'19|
//  +-----------------------------------------------------------------------------+
void RenderCore::SetInstance( const int instanceIdx, const int meshIdx, const mat4& matrix )
{
	// Note: for first-time setup, meshes are expected to be passed in sequential order.
	// This will result in new CoreInstance pointers being pushed into the instances vector.
	// Subsequent instance changes (typically: transforms) will be applied to existing CoreInstances.
	if (instanceIdx >= instances.size())
	{
		instances.push_back( new CoreInstance() );
		// create a geometry instance
		meshes[meshIdx]->geometryTriangles->validate();
		instances[instanceIdx]->geometryInstance = context->createGeometryInstance( meshes[meshIdx]->geometryTriangles, dummyMaterial );
		instances[instanceIdx]->geometryInstance["instanceIndex"]->setInt( instanceIdx );
		// put the geometry instance in a geometry group
		instances[instanceIdx]->geometryGroup = context->createGeometryGroup();
		instances[instanceIdx]->geometryGroup->addChild( instances[instanceIdx]->geometryInstance );
		instances[instanceIdx]->geometryGroup->setAcceleration( context->createAcceleration( "Trbvh" ) );
		// set a transform for the geometry group
		instances[instanceIdx]->transform = context->createTransform();
		instances[instanceIdx]->transform->setChild( instances[instanceIdx]->geometryGroup );
	}
	// update the matrices for the transform
	mat4 inverted = matrix;
	inverted.Inverted();
	if (instances[instanceIdx]->transform) instances[instanceIdx]->transform->setMatrix( false /* flag: transpose */, (const float*)&matrix, (const float*)&inverted );
	instances[instanceIdx]->mesh = meshIdx;
	// mark the toplevel as dirty
	topLevelGroup->getAcceleration()->markDirty();
}

//  +-----------------------------------------------------------------------------+
//  |  RenderCore::UpdateToplevel                                                 |
//  |  After changing meshes, instances or instance transforms, we need to        |
//  |  rebuild the top-level structure.                                     LH2'19|
//  +-----------------------------------------------------------------------------+
void RenderCore::UpdateToplevel()
{
	// see if topLevelGroup is large enough for current scene
	if (topLevelGroup->getChildCount() != instances.size()) topLevelGroup->setChildCount( (int)instances.size() );
	// set the topLevelGroup children
	for (int s = (int)instances.size(), i = 0; i < s; i++) topLevelGroup->setChild( i, instances[i]->transform );
}

//  +-----------------------------------------------------------------------------+
//  |  RenderCore::SetTextures                                                    |
//  |  Set the texture data.                                                LH2'19|
//  +-----------------------------------------------------------------------------+
void RenderCore::SetTextures( const CoreTexDesc* tex, const int textures )
{
	// copy the supplied array of texture descriptors
	delete texDescs; texDescs = 0;
	textureCount = textures;
	if (textureCount == 0) return; // scene has no textures
	texDescs = new CoreTexDesc[textureCount];
	memcpy( texDescs, tex, textureCount * sizeof( CoreTexDesc ) );
	// copy texels for each type to the device
	SyncStorageType( TexelStorage::ARGB32 );
	SyncStorageType( TexelStorage::ARGB128 );
	SyncStorageType( TexelStorage::NRM32 );
	// Notes: 
	// - the three types are copied from the original HostTexture pixel data (to which the
	//   descriptors point) straight to the GPU. There is no pixel storage on the host
	//   in the RenderCore.
	// - the types are copied one by one. Copying involves creating a temporary host-side
	//   buffer; doing this one by one allows us to delete host-side data for one type
	//   before allocating space for the next, thus reducing storage requirements.
}

//  +-----------------------------------------------------------------------------+
//  |  RenderCore::SyncStorageType                                                |
//  |  Copies texel data for one storage type (argb32, argb128 or nrm32) to the   |
//  |  device. Note that this data is obtained from the original HostTexture      |
//  |  texel arrays.                                                        LH2'19|
//  +-----------------------------------------------------------------------------+
void RenderCore::SyncStorageType( const TexelStorage storage )
{
	uint texelTotal = 0;
	for (int i = 0; i < textureCount; i++) if (texDescs[i].storage == storage) texelTotal += texDescs[i].pixelCount;
	texelTotal = max( 16, texelTotal ); // OptiX does not tolerate empty buffers...
	// construct the continuous arrays
	switch (storage)
	{
	case TexelStorage::ARGB32:
		delete texel32Buffer;
		texel32Buffer = new CoreBuffer<uint>( texelTotal, ON_HOST | ON_DEVICE );
		SetARGB32Pixels( texel32Buffer->DevPtr() );
		coreStats.argb32TexelCount = texelTotal;
		break;
	case TexelStorage::ARGB128:
		delete texel128Buffer;
		SetARGB128Pixels( (texel128Buffer = new CoreBuffer<float4>( texelTotal, ON_HOST | ON_DEVICE ))->DevPtr() );
		coreStats.argb128TexelCount = texelTotal;
		break;
	case TexelStorage::NRM32:
		delete normal32Buffer;
		SetNRM32Pixels( (normal32Buffer = new CoreBuffer<uint>( texelTotal, ON_HOST | ON_DEVICE ))->DevPtr() );
		coreStats.nrm32TexelCount = texelTotal;
		break;
	}
	// copy texel data to arrays
	texelTotal = 0;
	for (int i = 0; i < textureCount; i++) if (texDescs[i].storage == storage)
	{
		void* destination = 0;
		switch (storage)
		{
		case TexelStorage::ARGB32:  destination = texel32Buffer->HostPtr() + texelTotal; break;
		case TexelStorage::ARGB128: destination = texel128Buffer->HostPtr() + texelTotal; break;
		case TexelStorage::NRM32:   destination = normal32Buffer->HostPtr() + texelTotal; break;
		}
		memcpy( destination, texDescs[i].idata, texDescs[i].pixelCount * sizeof( uint ) );
		texDescs[i].firstPixel = texelTotal;
		texelTotal += texDescs[i].pixelCount;
	}
	// move to device
	if (storage == TexelStorage::ARGB32) if (texel32Buffer) texel32Buffer->MoveToDevice();
	if (storage == TexelStorage::ARGB128) if (texel128Buffer) texel128Buffer->MoveToDevice();
	if (storage == TexelStorage::NRM32) if (normal32Buffer) normal32Buffer->MoveToDevice();
}

//  +-----------------------------------------------------------------------------+
//  |  RenderCore::SetMaterials                                                   |
//  |  Set the material data.                                               LH2'19|
//  +-----------------------------------------------------------------------------+
void RenderCore::SetMaterials( CoreMaterial* mat, const CoreMaterialEx* matEx, const int materialCount )
{
	// Notes:
	// Call this after the textures have been set; CoreMaterials store the offset of each texture
	// in the continuous arrays; this data is valid only when textures are in sync.
	delete materialBuffer;
	delete hostMaterialBuffer;
	hostMaterialBuffer = new CoreMaterial[materialCount];
	memcpy( hostMaterialBuffer, mat, materialCount * sizeof( CoreMaterial ) );
	for (int i = 0; i < materialCount; i++)
	{
		CoreMaterial& m = hostMaterialBuffer[i];
		const CoreMaterialEx& e = matEx[i];
		if (e.texture[0] != -1) m.texaddr0 = texDescs[e.texture[0]].firstPixel;
		if (e.texture[1] != -1) m.texaddr1 = texDescs[e.texture[1]].firstPixel;
		if (e.texture[2] != -1) m.texaddr2 = texDescs[e.texture[2]].firstPixel;
		if (e.texture[3] != -1) m.nmapaddr0 = texDescs[e.texture[3]].firstPixel;
		if (e.texture[4] != -1) m.nmapaddr1 = texDescs[e.texture[4]].firstPixel;
		if (e.texture[5] != -1) m.nmapaddr2 = texDescs[e.texture[5]].firstPixel;
		if (e.texture[6] != -1) m.smapaddr = texDescs[e.texture[6]].firstPixel;
		if (e.texture[7] != -1) m.rmapaddr = texDescs[e.texture[7]].firstPixel;
		// if (e.texture[ 8] != -1) m.texaddr0 = texDescs[e.texture[ 8]].firstPixel; second roughness map is not used
		if (e.texture[9] != -1) m.cmapaddr = texDescs[e.texture[9]].firstPixel;
		if (e.texture[10] != -1) m.amapaddr = texDescs[e.texture[10]].firstPixel;
	}
	materialBuffer = new CoreBuffer<CoreMaterial>( materialCount, ON_DEVICE | ON_HOST /* on_host: for alpha mapped tris */, hostMaterialBuffer );
	SetMaterialList( materialBuffer->DevPtr() );
}

//  +-----------------------------------------------------------------------------+
//  |  RenderCore::SetLights                                                      |
//  |  Set the light data.                                                  LH2'19|
//  +-----------------------------------------------------------------------------+
void RenderCore::SetLights( const CoreLightTri* areaLights, const int areaLightCount,
	const CorePointLight* pointLights, const int pointLightCount,
	const CoreSpotLight* spotLights, const int spotLightCount,
	const CoreDirectionalLight* directionalLights, const int directionalLightCount )
{
	delete areaLightBuffer;
	delete pointLightBuffer;
	delete spotLightBuffer;
	delete directionalLightBuffer;
	SetAreaLights( (areaLightBuffer = new CoreBuffer<CoreLightTri>( areaLightCount, ON_DEVICE, areaLights ))->DevPtr() );
	SetPointLights( (pointLightBuffer = new CoreBuffer<CorePointLight>( pointLightCount, ON_DEVICE, pointLights ))->DevPtr() );
	SetSpotLights( (spotLightBuffer = new CoreBuffer<CoreSpotLight>( spotLightCount, ON_DEVICE, spotLights ))->DevPtr() );
	SetDirectionalLights( (directionalLightBuffer = new CoreBuffer<CoreDirectionalLight>( directionalLightCount, ON_DEVICE, directionalLights ))->DevPtr() );
	SetLightCounts( areaLightCount, pointLightCount, spotLightCount, directionalLightCount );
}

//  +-----------------------------------------------------------------------------+
//  |  RenderCore::SetSkyData                                                     |
//  |  Set the sky dome data.                                               LH2'19|
//  +-----------------------------------------------------------------------------+
void RenderCore::SetSkyData( const float3* pixels, const uint width, const uint height )
{
	delete skyPixelBuffer;
	skyPixelBuffer = new CoreBuffer<float3>( width * height, ON_DEVICE, pixels );
	SetSkyPixels( skyPixelBuffer->DevPtr() );
	SetSkySize( width, height );
	skywidth = width;
	skyheight = height;
}

//  +-----------------------------------------------------------------------------+
//  |  RenderCore::Setting                                                        |
//  |  Modify a render setting.                                             LH2'19|
//  +-----------------------------------------------------------------------------+
void RenderCore::Setting( const char* name, const float value )
{
	if (!strcmp( name, "epsilon" ))
	{
		if (vars.geometryEpsilon != value)
		{
			vars.geometryEpsilon = value;
			SetGeometryEpsilon( value );
			context["geometryEpsilon"]->setFloat( value );
		}
	}
	else if (!strcmp( name, "clampValue" ))
	{
		if (vars.clampValue != value)
		{
			vars.clampValue = value;
			SetClampValue( value );
		}
	}
}

//  +-----------------------------------------------------------------------------+
//  |  RenderCore::Render                                                         |
//  |  Produce one image.                                                   LH2'19|
//  +-----------------------------------------------------------------------------+
void RenderCore::Render( const ViewPyramid& view, const Convergence converge, const float brightness, const float contrast )
{
	// wait for OpenGL
	glFinish();
	Timer timer;
	// clean accumulator, if requested
	if (converge == Restart)
	{
		accumulator->Clear( ON_DEVICE );
		samplesTaken = 0;
		camRNGseed = 0x12345678; // same seed means same noise.
	}
	// update instance descriptor array on device
	// Note: we are not using the built-in OptiX instance system for shading. Instead,
	// we figure out which triangle we hit, and to what instance it belongs; from there,
	// we handle normal management and material acquisition in custom code.
	if (instancesDirty)
	{
		// prepare CoreInstanceDesc array. For any sane number of instances this should
		// be efficient while yielding supreme flexibility.
		vector<CoreInstanceDesc> instDescArray;
		for (auto instance : instances)
		{
			CoreInstanceDesc id;
			id.triangles = meshes[instance->mesh]->triangles->DevPtr();
			mat4 T, invT;
			if (instance->transform)
			{
				instance->transform->getMatrix( false, (float*)&T, (float*)&invT );
			}
			else
			{
				invT = mat4::Identity();
			}
			id.invTransform = *(float4x4*)&invT;
			instDescArray.push_back( id );
		}
		if (instDescBuffer == 0 || instDescBuffer->GetSize() < (int)instances.size())
		{
			delete instDescBuffer;
			// size of instance list changed beyond capacity.
			// Allocate a new buffer, with some slack, to prevent excessive reallocs.
			instDescBuffer = new CoreBuffer<CoreInstanceDesc>( instances.size() * 2, ON_HOST | ON_DEVICE );
			SetInstanceDescriptors( instDescBuffer->DevPtr() );
		}
		memcpy( instDescBuffer->HostPtr(), instDescArray.data(), instDescArray.size() * sizeof( CoreInstanceDesc ) );
		instDescBuffer->CopyToDevice();
		// instancesDirty = false;
	}
	// render image
	coreStats.totalExtensionRays = coreStats.totalShadowRays = 0;
	float3 right = view.p2 - view.p1, up = view.p3 - view.p1;
	// render an image using OptiX
	context["posLensSize"]->setFloat( view.pos.x, view.pos.y, view.pos.z, view.aperture );
	context["right"]->setFloat( right.x, right.y, right.z );
	context["up"]->setFloat( up.x, up.y, up.z );
	context["p1"]->setFloat( view.p1.x, view.p1.y, view.p1.z );
	context["pass"]->setUint( samplesTaken );
	// loop
	Counters counters;
	coreStats.deepRayCount = 0;
	coreStats.traceTimeX = coreStats.shadeTime = 0;
	uint pathCount = scrwidth * scrheight * scrspp;
	for (int pathLength = 1; pathLength <= 3; pathLength++)
	{
		// generate / extend
		Timer t;
		if (pathLength == 1)
		{
			// spawn and extend camera rays
			context["phase"]->setUint( 0 );
			coreStats.primaryRayCount = pathCount;
			InitCountersForExtend( pathCount );
			context->launch( 0, pathCount );
		}
		else
		{
			// extend bounced paths
			context["phase"]->setUint( 1 );
			if (pathLength == 2) coreStats.bounce1RayCount = pathCount;
			else coreStats.deepRayCount += pathCount;
			counterBuffer->CopyToHost();
			Counters& counters = counterBuffer->HostPtr()[0];
			InitCountersSubsequent();
			context->launch( 0, pathCount );
		}
		if (pathLength == 1) coreStats.traceTime0 = t.elapsed();
		else if (pathLength == 2) coreStats.traceTime1 = t.elapsed();
		else coreStats.traceTimeX += t.elapsed();
		// shade
		cudaEventRecord( shadeStart[pathLength - 1] );
		shade( pathCount, accumulator->DevPtr(), scrwidth * scrheight * scrspp,
			pathStateBuffer->DevPtr(), hitBuffer->DevPtr(), connectionBuffer->DevPtr(),
			RandomUInt( camRNGseed ), blueNoise->DevPtr(), samplesTaken,
			probePos.x + scrwidth * probePos.y, pathLength, scrwidth, scrheight,
			view.spreadAngle, view.p1, view.p2, view.p3, view.pos );
		cudaEventRecord( shadeEnd[pathLength - 1] );
		counterBuffer->CopyToHost();
		counters = counterBuffer->HostPtr()[0];
		pathCount = counters.extensionRays;
	}
	// connect to light sources
	Timer t;
	context["phase"]->setUint( 2 );
	context->launch( 0, counters.shadowRays );
	coreStats.shadowTraceTime = t.elapsed();
	// gather ray tracing statistics
	coreStats.totalShadowRays = counters.shadowRays;
	coreStats.totalExtensionRays = counters.totalExtensionRays;
	// present accumulator to final buffer
	renderTarget.BindSurface();
	samplesTaken += scrspp;
	finalizeRender( accumulator->DevPtr(), scrwidth, scrheight, samplesTaken, brightness, contrast );
	renderTarget.UnbindSurface();
	// finalize statistics
	cudaStreamSynchronize( 0 );
	coreStats.renderTime = timer.elapsed();
	coreStats.totalRays = coreStats.totalExtensionRays + coreStats.totalShadowRays;
	for( int i = 0; i < MAXPATHLENGTH; i++ ) coreStats.shadeTime += CUDATools::Elapsed( shadeStart[i], shadeEnd[i] );
	coreStats.probedInstid = counters.probedInstid;
	coreStats.probedTriid = counters.probedTriid;
	coreStats.probedDist = counters.probedDist;
}

//  +-----------------------------------------------------------------------------+
//  |  RenderCore::Shutdown                                                       |
//  |  Free all resources.                                                  LH2'19|
//  +-----------------------------------------------------------------------------+
void RenderCore::Shutdown()
{
	context->destroy();
}

// EOF