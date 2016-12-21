// script.cpp : Defines the exported functions for the DLL application.
//

#include "stdafx.h"
#include "custom-hooks.h"
#include "script.h"
#include "MFUtility.h"
#include "encoder.h"
#include "logger.h"
#include "config.h"
#include "util.h"
#include "yara-helper.h"
#include "game-detour-def.h"

#include "..\DirectXTex\DirectXTex\DirectXTex.h"
#include "hook-def.h"

using namespace Microsoft::WRL;

namespace {
	std::shared_ptr<PLH::VFuncDetour> hkIMFTransform_ProcessInput(new PLH::VFuncDetour);
	std::shared_ptr<PLH::VFuncDetour> hkIMFTransform_ProcessMessage(new PLH::VFuncDetour);
	std::shared_ptr<PLH::VFuncDetour> hkIMFSinkWriter_AddStream(new PLH::VFuncDetour);
	std::shared_ptr<PLH::VFuncDetour> hkIMFSinkWriter_SetInputMediaType(new PLH::VFuncDetour);
	std::shared_ptr<PLH::VFuncDetour> hkIMFSinkWriter_WriteSample(new PLH::VFuncDetour);
	std::shared_ptr<PLH::VFuncDetour> hkIMFSinkWriter_Finalize(new PLH::VFuncDetour);
	std::shared_ptr<PLH::VFuncDetour> hkOMSetRenderTargets(new PLH::VFuncDetour);
	std::shared_ptr<PLH::VFuncDetour> hkCreateRenderTargetView(new PLH::VFuncDetour);
	std::shared_ptr<PLH::VFuncDetour> hkCreateDepthStencilView(new PLH::VFuncDetour);
	std::shared_ptr<PLH::VFuncDetour> hkCreateTexture2D(new PLH::VFuncDetour);
	std::shared_ptr<PLH::VFuncDetour> hkRSSetViewports(new PLH::VFuncDetour);
	std::shared_ptr<PLH::VFuncDetour> hkRSSetScissorRects(new PLH::VFuncDetour);
	
	
	
	std::shared_ptr<PLH::IATHook> hkCoCreateInstance(new PLH::IATHook);
	std::shared_ptr<PLH::IATHook> hkMFCreateSinkWriterFromURL(new PLH::IATHook);

	std::shared_ptr<PLH::X64Detour> hkGetFrameRateFraction(new PLH::X64Detour);
	std::shared_ptr<PLH::X64Detour> hkGetRenderTimeBase(new PLH::X64Detour);
	std::shared_ptr<PLH::X64Detour> hkGetFrameRate(new PLH::X64Detour);
	std::shared_ptr<PLH::X64Detour> hkGetAudioSamples(new PLH::X64Detour);
	std::shared_ptr<PLH::X64Detour> hkUnk01(new PLH::X64Detour);

	std::unique_ptr<Encoder::Session> session;
	std::mutex mxSession;

	std::thread::id mainThreadId;
	ComPtr<IDXGISwapChain> mainSwapChain;

	ComPtr<IDXGIFactory> pDXGIFactory;

	struct ExportContext {

		ExportContext()
		{
			PRE();			
			POST();
		}

		~ExportContext() {
			PRE();
			POST();
		}

		bool isAudioExportDisabled = false;

		bool captureRenderTargetViewReference = false;
		bool captureDepthStencilViewReference = false;
		
		ComPtr<ID3D11RenderTargetView> pExportRenderTargetView;
		ComPtr<ID3D11Texture2D> pExportRenderTarget;
		ComPtr<ID3D11Texture2D> pExportDepthStencil;
		ComPtr<ID3D11DeviceContext> pDeviceContext;
		ComPtr<ID3D11Device> pDevice;
		ComPtr<IDXGIFactory> pDXGIFactory;
		ComPtr<IDXGISwapChain> pSwapChain;

		std::shared_ptr<DirectX::ScratchImage> capturedImage = std::make_shared<DirectX::ScratchImage>();

		UINT pts = 0;

		ComPtr<IMFMediaType> videoMediaType;
	};

	std::shared_ptr<ExportContext> exportContext;

	std::shared_ptr<YaraHelper> pYaraHelper;

}

tCoCreateInstance oCoCreateInstance;
tMFCreateSinkWriterFromURL oMFCreateSinkWriterFromURL;
tIMFSinkWriter_SetInputMediaType oIMFSinkWriter_SetInputMediaType;
tIMFSinkWriter_AddStream oIMFSinkWriter_AddStream;
tIMFSinkWriter_WriteSample oIMFSinkWriter_WriteSample;
tIMFSinkWriter_Finalize oIMFSinkWriter_Finalize;
tOMSetRenderTargets oOMSetRenderTargets;
tCreateTexture2D oCreateTexture2D;
tCreateRenderTargetView oCreateRenderTargetView;
tCreateDepthStencilView oCreateDepthStencilView;
tRSSetViewports oRSSetViewports;
tRSSetScissorRects oRSSetScissorRects;
tGetRenderTimeBase oGetRenderTimeBase;
//tGetFrameRate oGetFrameRate;
//tGetAudioSamples oGetAudioSamples;
//tgetFrameRateFraction ogetFrameRateFraction;
//tUnk01 oUnk01;

void avlog_callback(void *ptr, int level, const char* fmt, va_list vargs) {
	static char msg[8192];
	vsnprintf_s(msg, sizeof(msg), fmt, vargs);
	Logger::instance().write(
		Logger::instance().getTimestamp(),
		" ",
		Logger::instance().getLogLevelString(LL_NON),
		" ",
		Logger::instance().getThreadId(), " AVCODEC: ");
	if (ptr)
	{
		AVClass *avc = *(AVClass**)ptr;
		Logger::instance().write("(");
		Logger::instance().write(avc->item_name(ptr));
		Logger::instance().write("): ");
	}
	Logger::instance().write(msg);
}

void onPresent(IDXGISwapChain *swapChain) {
	mainSwapChain = swapChain;
	static bool initialized = false;
	if (!initialized) {
		initialized = true;
		try {
			ComPtr<ID3D11Device> pDevice;
			ComPtr<ID3D11DeviceContext> pDeviceContext;
			ComPtr<ID3D11Texture2D> texture;
			DXGI_SWAP_CHAIN_DESC desc;


			swapChain->GetDesc(&desc);


			LOG(LL_NFO, "BUFFER COUNT: ", desc.BufferCount);
			REQUIRE(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)texture.GetAddressOf()), "Failed to get the texture buffer");
			REQUIRE(swapChain->GetDevice(__uuidof(ID3D11Device), (void**)pDevice.GetAddressOf()), "Failed to get the D3D11 device");
			pDevice->GetImmediateContext(pDeviceContext.GetAddressOf());
			NOT_NULL(pDeviceContext.Get(), "Failed to get D3D11 device context");
			REQUIRE(hookVirtualFunction(pDevice.Get(), 5, &Hook_CreateTexture2D, &oCreateTexture2D, hkCreateTexture2D), "Failed to hook ID3DDeviceContext::Map");
			REQUIRE(hookVirtualFunction(pDevice.Get(), 9, &Hook_CreateRenderTargetView, &oCreateRenderTargetView, hkCreateRenderTargetView), "Failed to hook ID3DDeviceContext::CreateRenderTargetView");
			REQUIRE(hookVirtualFunction(pDevice.Get(), 10, &Hook_CreateDepthStencilView, &oCreateDepthStencilView, hkCreateDepthStencilView), "Failed to hook ID3DDeviceContext::CreateDepthStencilView");
			REQUIRE(hookVirtualFunction(pDeviceContext.Get(), 33, &Hook_OMSetRenderTargets, &oOMSetRenderTargets, hkOMSetRenderTargets), "Failed to hook ID3DDeviceContext::OMSetRenderTargets");
			REQUIRE(hookVirtualFunction(pDeviceContext.Get(), 44, &RSSetViewports, &oRSSetViewports, hkRSSetViewports), "Failed to hook ID3DDeviceContext::RSSetViewports"); 
			REQUIRE(hookVirtualFunction(pDeviceContext.Get(), 45, &RSSetScissorRects, &oRSSetScissorRects, hkRSSetScissorRects), "Failed to hook ID3DDeviceContext::RSSetViewports"); 

			ComPtr<IDXGIDevice> pDXGIDevice;
			REQUIRE(pDevice.As(&pDXGIDevice), "Failed to get IDXGIDevice from ID3D11Device");
			
			ComPtr<IDXGIAdapter> pDXGIAdapter;
			REQUIRE(pDXGIDevice->GetParent(__uuidof(IDXGIAdapter), (void**)pDXGIAdapter.GetAddressOf()), "Failed to get IDXGIAdapter");

			REQUIRE(pDXGIAdapter->GetParent(__uuidof(IDXGIFactory), (void**)pDXGIFactory.GetAddressOf()), "Failed to get IDXGIFactory");

		} catch (std::exception& ex) {
			LOG(LL_ERR, ex.what());
		}
	}
}

void initialize() {
	PRE();
	try {
		mainThreadId = std::this_thread::get_id();

		REQUIRE(hookNamedFunction("mfreadwrite.dll", "MFCreateSinkWriterFromURL", &Hook_MFCreateSinkWriterFromURL, &oMFCreateSinkWriterFromURL, hkMFCreateSinkWriterFromURL), "Failed to hook MFCreateSinkWriterFromURL in mfreadwrite.dll");
		REQUIRE(hookNamedFunction("ole32.dll", "CoCreateInstance", &Hook_CoCreateInstance, &oCoCreateInstance, hkCoCreateInstance), "Failed to hook CoCreateInstance in ole32.dll");
			
		pYaraHelper.reset(new YaraHelper());
		pYaraHelper->initialize();
		pYaraHelper->performScan();

		MODULEINFO info;
		GetModuleInformation(GetCurrentProcess(), GetModuleHandle(NULL), &info, sizeof(info));
		LOG(LL_NFO, "Image base:", ((void*)info.lpBaseOfDll));

		//LOG(LL_NFO, (VOID*)(0x311C84 + (intptr_t)info.lpBaseOfDll));
		try {
			if (pYaraHelper->getPtr_getRenderTimeBase() != NULL) {
				REQUIRE(hookX64Function((LPVOID)pYaraHelper->getPtr_getRenderTimeBase(), &Detour_GetRenderTimeBase, &oGetRenderTimeBase, hkGetRenderTimeBase), "Failed to hook FPS function.");
				////0x14CC6AC;
				//REQUIRE(hookX64Function((BYTE*)(0x14CC6AC + (intptr_t)info.lpBaseOfDll), &Detour_GetFrameRate, &oGetFrameRate, hkGetFrameRate), "Failed to hook frame rate function.");
				////0x14CC64C;
				//REQUIRE(hookX64Function((BYTE*)(0x14CC64C + (intptr_t)info.lpBaseOfDll), &Detour_GetAudioSamples, &oGetAudioSamples, hkGetAudioSamples), "Failed to hook audio sample function.");
				//0x14CBED8;
				//REQUIRE(hookX64Function((BYTE*)(0x14CBED8 + (intptr_t)info.lpBaseOfDll), &Detour_getFrameRateFraction, &ogetFrameRateFraction, hkGetFrameRateFraction), "Failed to hook audio sample function.");
				//0x87CC80;
				//REQUIRE(hookX64Function((BYTE*)(0x87CC80 + (intptr_t)info.lpBaseOfDll), &Detour_Unk01, &oUnk01, hkUnk01), "Failed to hook audio sample function.");
			} else {
				LOG(LL_ERR, "Could not find the address for FPS function.");
				LOG(LL_ERR, "Custom FPS support is DISABLED!!!");
			}
		} catch (std::exception& ex) {
			LOG(LL_ERR, ex.what());
		}

		LOG_CALL(LL_DBG, av_register_all());
		LOG_CALL(LL_DBG, avcodec_register_all());
		LOG_CALL(LL_DBG, av_log_set_level(AV_LOG_TRACE));
		LOG_CALL(LL_DBG, av_log_set_callback(&avlog_callback));
	} catch (std::exception& ex) {
		// TODO cleanup
		POST();
		throw ex;
	}
	POST();
}

void ScriptMain() {
	PRE();
	LOG(LL_NFO, "Starting main loop");
	while (true) {
		WAIT(0);
	}
	POST();
}

static HRESULT Hook_CoCreateInstance(
	REFCLSID  rclsid,
	LPUNKNOWN pUnkOuter,
	DWORD     dwClsContext,
	REFIID    riid,
	LPVOID    *ppv
	) {
	PRE();
	HRESULT result = oCoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv);
	char buffer[64];
	GUIDToString(rclsid, buffer, 64);
	LOG(LL_NFO, "CoCreateInstance: ", buffer);
	POST();
	return result;
}

static HRESULT Hook_CreateRenderTargetView(
	ID3D11Device                  *pThis,
	ID3D11Resource                *pResource,
	const D3D11_RENDER_TARGET_VIEW_DESC *pDesc,
	ID3D11RenderTargetView        **ppRTView
	) {
	if ((exportContext != NULL) && exportContext->captureRenderTargetViewReference && (std::this_thread::get_id() == mainThreadId)) {
		try {
			LOG(LL_NFO, "Capturing export render target view...");
			exportContext->captureRenderTargetViewReference = false;
			exportContext->pDevice = pThis;
			exportContext->pDevice->GetImmediateContext(exportContext->pDeviceContext.GetAddressOf());

			ComPtr<ID3D11Texture2D> pTexture;
			REQUIRE(pResource->QueryInterface(pTexture.GetAddressOf()), "Failed to convert ID3D11Resource to ID3D11Texture2D");
			D3D11_TEXTURE2D_DESC desc;
			pTexture->GetDesc(&desc);

			/*DXGI_SWAP_CHAIN_DESC swapChainDesc;
			mainSwapChain->GetDesc(&swapChainDesc);*/

			/*desc.Width = swapChainDesc.BufferDesc.Width;
			desc.Height = swapChainDesc.BufferDesc.Height;*/

			LOG(LL_DBG, "Creating render target view: w:", desc.Width, " h:", desc.Height);

			/*ComPtr<ID3D11Texture2D> pTexture;
			pThis->CreateTexture2D(&desc, NULL, pTexture.GetAddressOf());
			ComPtr<ID3D11Resource> pRes;
			REQUIRE(pTexture.As(&pRes), "Failed to convert ID3D11Texture2D to ID3D11Resource");*/
			HRESULT result = oCreateRenderTargetView(pThis, pResource, pDesc, ppRTView);
			exportContext->pExportRenderTarget = pTexture;
			exportContext->pExportRenderTargetView = *(ppRTView);
			POST();
			return result;
		} catch (std::exception& ex) {
			LOG(LL_ERR, ex.what());
		}
	}
	return oCreateRenderTargetView(pThis, pResource, pDesc, ppRTView);
}

static HRESULT Hook_CreateDepthStencilView(
	ID3D11Device                  *pThis,
	ID3D11Resource                *pResource,
	const D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc,
	ID3D11DepthStencilView        **ppDepthStencilView
	) {
	if ((exportContext != NULL) && exportContext->captureDepthStencilViewReference && mainThreadId == std::this_thread::get_id()) {
		try {
			exportContext->captureDepthStencilViewReference = false;
			LOG(LL_NFO, "Capturing export depth stencil view...");
			ComPtr<ID3D11Texture2D> pTexture;
			REQUIRE(pResource->QueryInterface(pTexture.GetAddressOf()), "Failed to get depth stencil texture");
			//D3D11_TEXTURE2D_DESC desc;
			//pOldTexture->GetDesc(&desc);
			//ComPtr<ID3D11Texture2D> pTexture;
			//pThis->CreateTexture2D(&desc, NULL, pTexture.GetAddressOf());

			//ComPtr<ID3D11Resource> pRes;
			//REQUIRE(pTexture.As(&pRes), "Failed to convert ID3D11Texture to ID3D11Resource.");
			exportContext->pExportDepthStencil = pTexture;
			return oCreateDepthStencilView(pThis, pResource, pDesc, ppDepthStencilView);
		} catch (...) {
			LOG(LL_ERR, "Failed to capture depth stencil view");
		}
	}

	HRESULT result = oCreateDepthStencilView(pThis, pResource, pDesc, ppDepthStencilView);
	return result;
}

static void RSSetViewports(
	ID3D11DeviceContext  *pThis,
	UINT                 NumViewports,
	const D3D11_VIEWPORT *pViewports
	) {
	if ((exportContext != NULL) && isCurrentRenderTargetView(pThis, exportContext->pExportRenderTargetView)) {
		D3D11_VIEWPORT* pNewViewports = new D3D11_VIEWPORT[NumViewports];
		for (UINT i = 0; i < NumViewports; i++) {
			pNewViewports[i] = pViewports[i];
			DXGI_SWAP_CHAIN_DESC desc;
			exportContext->pSwapChain->GetDesc(&desc);
			pNewViewports[i].Width = desc.BufferDesc.Width;
			pNewViewports[i].Height = desc.BufferDesc.Height;
		}
		return oRSSetViewports(pThis, NumViewports, pNewViewports);
	}

	return oRSSetViewports(pThis, NumViewports, pViewports);
}

static void RSSetScissorRects(
	ID3D11DeviceContext *pThis,
	UINT          NumRects,
	const D3D11_RECT *  pRects
	) {
	if ((exportContext != NULL) && isCurrentRenderTargetView(pThis, exportContext->pExportRenderTargetView)) {
		D3D11_RECT* pNewRects = new D3D11_RECT[NumRects];
		for (UINT i = 0; i < NumRects; i++) {
			pNewRects[i] = pRects[i];
			DXGI_SWAP_CHAIN_DESC desc;
			exportContext->pSwapChain->GetDesc(&desc);
			pNewRects[i].right = desc.BufferDesc.Width;
			pNewRects[i].bottom = desc.BufferDesc.Height;
		}
		return oRSSetScissorRects(pThis, NumRects, pNewRects);
	}

	return oRSSetScissorRects(pThis, NumRects, pRects);
}

static void Hook_OMSetRenderTargets(
	ID3D11DeviceContext           *pThis,
	UINT                          NumViews,
	ID3D11RenderTargetView *const *ppRenderTargetViews,
	ID3D11DepthStencilView        *pDepthStencilView
	) {
	if ((exportContext != NULL) && isCurrentRenderTargetView(pThis, exportContext->pExportRenderTargetView)) {
		std::lock_guard<std::mutex> sessionLock(mxSession);
		if ((session != NULL) && (session->isCapturing)) {
			// Time to capture rendered frame
			try {

				LOG_CALL(LL_DBG,exportContext->pSwapChain->Present(0, DXGI_PRESENT_TEST));

				ComPtr<ID3D11Texture2D> pSwapChainBuffer;
				REQUIRE(exportContext->pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)pSwapChainBuffer.GetAddressOf()), "Failed to get swap chain's buffer");

				auto& image_ref = *(exportContext->capturedImage);
				LOG_CALL(LL_DBG, DirectX::CaptureTexture(exportContext->pDevice.Get(), exportContext->pDeviceContext.Get(), pSwapChainBuffer.Get(), image_ref));
				if (exportContext->capturedImage->GetImageCount() == 0) {
					LOG(LL_ERR, "There is no image to capture.");
					throw std::exception();
				}
				const DirectX::Image* image = exportContext->capturedImage->GetImage(0, 0, 0);
				NOT_NULL(image, "Could not get current frame.");
				NOT_NULL(image->pixels, "Could not get current frame.");

				REQUIRE(session->enqueueVideoFrame(image->pixels, (int)(image->width * image->height * 4)), "Failed to enqueue frame");
				exportContext->capturedImage->Release();
			} catch (std::exception&) {
				LOG(LL_ERR, "Reading video frame from D3D Device failed.");
				exportContext->capturedImage->Release();
				LOG_CALL(LL_DBG, session.reset());
				LOG_CALL(LL_DBG, exportContext.reset());
			}
		}
	}
	oOMSetRenderTargets(pThis, NumViews, ppRenderTargetViews, pDepthStencilView);
}

static HRESULT Hook_MFCreateSinkWriterFromURL(
	LPCWSTR       pwszOutputURL,
	IMFByteStream *pByteStream,
	IMFAttributes *pAttributes,
	IMFSinkWriter **ppSinkWriter
	) {
	PRE();
	HRESULT result = oMFCreateSinkWriterFromURL(pwszOutputURL, pByteStream, pAttributes, ppSinkWriter);
	if (SUCCEEDED(result)) {
		try {
			REQUIRE(hookVirtualFunction(*ppSinkWriter, 3, &Hook_IMFSinkWriter_AddStream, &oIMFSinkWriter_AddStream, hkIMFSinkWriter_AddStream), "Failed to hook IMFSinkWriter::AddStream");
			REQUIRE(hookVirtualFunction(*ppSinkWriter, 4, &IMFSinkWriter_SetInputMediaType, &oIMFSinkWriter_SetInputMediaType, hkIMFSinkWriter_SetInputMediaType), "Failed to hook IMFSinkWriter::SetInputMediaType");
			REQUIRE(hookVirtualFunction(*ppSinkWriter, 6, &Hook_IMFSinkWriter_WriteSample, &oIMFSinkWriter_WriteSample, hkIMFSinkWriter_WriteSample), "Failed to hook IMFSinkWriter::WriteSample");
			REQUIRE(hookVirtualFunction(*ppSinkWriter, 11, &Hook_IMFSinkWriter_Finalize, &oIMFSinkWriter_Finalize, hkIMFSinkWriter_Finalize), "Failed to hook IMFSinkWriter::Finalize");
		} catch (...) {
			LOG(LL_ERR, "Hooking IMFSinkWriter functions failed");
		}
	}
	POST();
	return result;
}

static HRESULT Hook_IMFSinkWriter_AddStream(
	IMFSinkWriter *pThis,
	IMFMediaType  *pTargetMediaType,
	DWORD         *pdwStreamIndex
	) {
	PRE();
	LOG(LL_NFO, "IMFSinkWriter::AddStream: ", GetMediaTypeDescription(pTargetMediaType).c_str());
	POST();
	return oIMFSinkWriter_AddStream(pThis, pTargetMediaType, pdwStreamIndex);
}

static HRESULT IMFSinkWriter_SetInputMediaType(
	IMFSinkWriter *pThis,
	DWORD         dwStreamIndex,
	IMFMediaType  *pInputMediaType,
	IMFAttributes *pEncodingParameters
	) {
	PRE();
	LOG(LL_NFO, "IMFSinkWriter::SetInputMediaType: ", GetMediaTypeDescription(pInputMediaType).c_str());

	GUID majorType;
	if (SUCCEEDED(pInputMediaType->GetMajorType(&majorType))) {
		if (IsEqualGUID(majorType, MFMediaType_Video)) {
			exportContext->videoMediaType = pInputMediaType;
		} else if (IsEqualGUID(majorType, MFMediaType_Audio)) {
			try {
				std::lock_guard<std::mutex> sessionLock(mxSession);

				// Create Video Context
				{
					UINT width, height, fps_num, fps_den;
					MFGetAttribute2UINT32asUINT64(exportContext->videoMediaType.Get(), MF_MT_FRAME_SIZE, &width, &height);
					//MFGetAttributeRatio(exportContext->videoMediaType.Get(), MF_MT_FRAME_RATE, &fps_num, &fps_den);

					GUID pixelFormat;
					exportContext->videoMediaType->GetGUID(MF_MT_SUBTYPE, &pixelFormat);

					DXGI_SWAP_CHAIN_DESC desc;
					exportContext->pSwapChain->GetDesc(&desc);

					auto fps = Config::instance().getFPS();

					if (((float)fps.first * ((float)Config::instance().getMotionBlurSamples() + 1) / (float)fps.second) > 60.0f) {
						LOG(LL_NON, "fps * (motion_blur_samples + 1) > 60.0!!!");
						LOG(LL_NON, "Audio export will be disabled!!!");
						exportContext->isAudioExportDisabled = true;
					}

					REQUIRE(session->createVideoContext(desc.BufferDesc.Width, desc.BufferDesc.Height, "bgra", fps.first, fps.second, Config::instance().getMotionBlurSamples(),  Config::instance().videoFmt(), Config::instance().videoEnc(), Config::instance().videoCfg()), "Failed to create video context");
				}
				
				// Create Audio Context
				{
					UINT32 blockAlignment, numChannels, sampleRate, bitsPerSample;
					GUID subType;

					pInputMediaType->GetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, &blockAlignment);
					pInputMediaType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &numChannels);
					pInputMediaType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate);
					pInputMediaType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bitsPerSample);
					pInputMediaType->GetGUID(MF_MT_SUBTYPE, &subType);

					if (IsEqualGUID(subType, MFAudioFormat_PCM)) {
						REQUIRE(session->createAudioContext(numChannels, sampleRate, bitsPerSample, AV_SAMPLE_FMT_S16, blockAlignment, Config::instance().audioRate(), Config::instance().audioFmt(), Config::instance().audioEnc(), Config::instance().audioCfg()), "Failed to create audio context.");
					} else {
						char buffer[64];
						GUIDToString(subType, buffer, 64);
						LOG(LL_ERR, "Unsupported input audio format: ", buffer);
						throw std::runtime_error("Unsupported input audio format");
					}
				}

				// Create Format Context
				{
					char buffer[128];
					std::string output_file = Config::instance().outputDir();

					output_file += "\\XVX-";
					time_t rawtime;
					struct tm timeinfo;
					time(&rawtime);
					localtime_s(&timeinfo, &rawtime);
					strftime(buffer, 128, "%Y%m%d%H%M%S", &timeinfo);
					output_file += buffer;
					output_file += "." + Config::instance().getContainerFormat();

					std::string filename = std::regex_replace(output_file, std::regex("\\\\+"), "\\");

					LOG(LL_NFO, "Output file: ", filename);

					REQUIRE(session->createFormatContext(filename.c_str()), "Failed to create format context");
				}
			} catch (std::exception&) {
				LOG_CALL(LL_DBG, session.reset());
				LOG_CALL(LL_DBG, exportContext.reset());
			}
		}
	}

	POST();
	return oIMFSinkWriter_SetInputMediaType(pThis, dwStreamIndex, pInputMediaType, pEncodingParameters);
}

static HRESULT Hook_IMFSinkWriter_WriteSample(
	IMFSinkWriter *pThis,
	DWORD         dwStreamIndex,
	IMFSample     *pSample
	) {
	std::lock_guard<std::mutex> sessionLock(mxSession);
	if ((session != NULL) && (dwStreamIndex == 1) && (!exportContext->isAudioExportDisabled)) {

		static std::ofstream stream = std::ofstream("t:\\temp.raw", std::ofstream::app | std::ofstream::binary);

		ComPtr<IMFMediaBuffer> pBuffer = NULL;
		try {
			LONGLONG sampleTime;
			pSample->GetSampleTime(&sampleTime);
			pSample->ConvertToContiguousBuffer(pBuffer.GetAddressOf());

			DWORD length;
			pBuffer->GetCurrentLength(&length);
			BYTE *buffer;
			if (SUCCEEDED(pBuffer->Lock(&buffer, NULL, NULL))) {
				stream.write((char*)buffer, length);
				LOG_CALL(LL_DBG, session->writeAudioFrame(buffer, length, sampleTime));
			}
			
		} catch (std::exception& ex) {
			LOG(LL_ERR, ex.what());
			LOG_CALL(LL_DBG, session.reset());
			LOG_CALL(LL_DBG, exportContext.reset());
			if (pBuffer != NULL) {
				pBuffer->Unlock();
			}
		}
	}
	return S_OK;
}

static HRESULT Hook_IMFSinkWriter_Finalize(
	IMFSinkWriter *pThis
	) {
	PRE();
	std::lock_guard<std::mutex> sessionLock(mxSession);
	try {
		if (session != NULL) {
			LOG_CALL(LL_DBG, session->finishAudio());
			LOG_CALL(LL_DBG, session->finishVideo());
			LOG_CALL(LL_DBG, session->endSession());
		}
	} catch (std::exception& ex) {
		LOG(LL_ERR, ex.what());
	}

	LOG_CALL(LL_DBG, session.reset());
	LOG_CALL(LL_DBG, exportContext.reset());
	POST();
	return S_OK;
}

void finalize() {
	PRE();
	hkCoCreateInstance->UnHook();
	hkMFCreateSinkWriterFromURL->UnHook();
	hkIMFTransform_ProcessInput->UnHook();
	hkIMFTransform_ProcessMessage->UnHook();
	hkIMFSinkWriter_AddStream->UnHook();
	hkIMFSinkWriter_SetInputMediaType->UnHook();
	hkIMFSinkWriter_WriteSample->UnHook();
	hkIMFSinkWriter_Finalize->UnHook();
	POST();
}

static HRESULT Hook_CreateTexture2D(
	ID3D11Device*				 *pThis,
	const D3D11_TEXTURE2D_DESC   *pDesc,
	const D3D11_SUBRESOURCE_DATA *pInitialData,
	ID3D11Texture2D        **ppTexture2D
	) {
	// Detect export buffer creation
	if (pDesc->Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
		LOG(LL_NFO, "ID3D11Device::CreateTexture2D: fmt:", conv_dxgi_format_to_string(pDesc->Format),
			" w:", pDesc->Width,
			" h:", pDesc->Height);
	}
	if (pDesc && (pDesc->CPUAccessFlags & D3D11_CPU_ACCESS_READ) && (std::this_thread::get_id() == mainThreadId) && !((pDesc->Width == 512) && (pDesc->Height == 256)) && !((pDesc->Width == 4) && (pDesc->Height == 4)) && (pDesc->Format == DXGI_FORMAT_B8G8R8A8_UNORM)) {
		DXGI_SWAP_CHAIN_DESC swapChainDesc;
		REQUIRE(mainSwapChain->GetDesc(&swapChainDesc), "Failed to get swap chain descriptor");

		LOG(LL_NFO, "ID3D11Device::CreateTexture2D: fmt:", conv_dxgi_format_to_string(pDesc->Format),
			" w:", pDesc->Width,
			" h:", pDesc->Height);
		/*LOG(LL_NFO, swapChainDesc.BufferDesc.Width, " ", swapChainDesc.BufferDesc.Height);*/
		//if (((abs(int(pDesc->Width - swapChainDesc.BufferDesc.Width)) < 32.0) && (abs(int(pDesc->Height - swapChainDesc.BufferDesc.Height)) < 32.0))) {
			std::lock_guard<std::mutex> sessionLock(mxSession);
			LOG_CALL(LL_DBG, exportContext.reset());
			LOG_CALL(LL_DBG, session.reset());
			try {
				LOG(LL_NFO, "Creating session...");
				
				if (Config::instance().isAutoReloadEnabled()) {
					LOG_CALL(LL_DBG, Config::instance().reload());
				}

				session.reset(new Encoder::Session());
				NOT_NULL(session, "Could not create the session");
				exportContext.reset(new ExportContext());
				NOT_NULL(exportContext, "Could not create export context");
				exportContext->pSwapChain = mainSwapChain;
				exportContext->captureRenderTargetViewReference = true;
				exportContext->captureDepthStencilViewReference = true;
				/*D3D11_TEXTURE2D_DESC* desc = (D3D11_TEXTURE2D_DESC*)pDesc;
				desc->Width = swapChainDesc.BufferDesc.Width;
				desc->Height = swapChainDesc.BufferDesc.Height;
				return oCreateTexture2D(pThis, &desc, pInitialData, ppTexture2D);*/
			}
			catch (std::exception&) {
				LOG_CALL(LL_DBG, session.reset());
				LOG_CALL(LL_DBG, exportContext.reset());
			}
		//}
	}

	return oCreateTexture2D(pThis, pDesc, pInitialData, ppTexture2D);
}

static float Detour_GetRenderTimeBase(int64_t choice) {
	StackDump(64, __func__);
	std::pair<int32_t, int32_t> fps = Config::instance().getFPS();
	float result = 1000.0f * (float)fps.second / ((float)fps.first * ((float)Config::instance().getMotionBlurSamples() + 1));
	LOG(LL_NFO, "Time step: ", result);
	return result;
}

//static float Detour_GetFrameRate(int32_t choice) {
//	std::pair<int32_t, int32_t> fps = Config::instance().getFPS();
//	return (float)fps.first / (float)fps.second;
//}
//
//static void Detour_getFrameRateFraction(float input, uint32_t* num, uint32_t* den) {
//	ogetFrameRateFraction(input, num, den);
//	*num = 120;
//	*den = 1;
//	LOG(LL_DBG, input, " ", *num, "/", *den);
//}
//
//static float Detour_Unk01(float x0, float x1, float x2, float x3) {
//	float result = oUnk01(x0, x1, x2, x3);
//	//LOG(LL_DBG, "(", x0, ",", x1, ",", x2, ",", x3, ") -> ", result);
//	return result;
//}