#pragma once

#include "../../Globals.cpp"
#include "ConsoleMgr.hpp"
#include <mmreg.h>
#include <vector>
#include <mutex>
#include <atomic>
#include <algorithm>

// =======================
// FixMissingSounds
// =======================
//
// Restores sounds that go missing when hardware mixing is enabled through a DirectSound wrapper (DSOAL).
// Only active together with FixSoundWrapperLoading, since it works through the DirectSound8 proxy.
//
// 1. Stereo 3D sounds: SndDrv.dll's CDx8SoundSys::Init3DSampleFromFile rejects any file with more than
//    one channel when hardware 3D is in use, so stereo dialogue (e.g. radio lines) is silently skipped.
//    The check is removed, and since DirectSound8 itself does not allow multi-channel 3D buffers, the
//    proxy creates a mono 3D buffer instead and downmixes the game's stereo data into it.
//
//    The check is located by its exact bytes for the known builds, or else by its structure
//    (channel check, then the PCM / ADPCM / MP3 format tag checks), so other builds are covered too.
//    The patch is retried on every device and buffer creation until SndDrv.dll is loaded.
//
// 2. Hardware voice exhaustion: every sound is created as LOCHARDWARE and idle buffers are kept alive,
//    so busy scenes exhaust the wrapper's hardware voice pool (128 in DSOAL) and new sounds are dropped.
//    Failed hardware buffers are retried as location-agnostic buffers, which DSOAL serves from its
//    software pool with the same 3D, HRTF and EAX support.
//
// Failures to apply show a popup. Events are printed to the console when ConsoleEnabled and DebugLevel
// are set: the first occurrences of each event in full, then running totals with the patch status.

namespace DirectSoundHelper
{
	// =======================
	// Logging and error reporting
	// =======================

	inline void LogMissingSounds(const std::string& text, ConsoleLineType type = ConsoleLineType::Info)
	{
		if (ConsoleEnabled && DebugLevel != 0)
			Console::Print("FixMissingSounds: " + text, type);
	}

	inline void ShowMissingSoundsError(const std::string& text)
	{
		LogMissingSounds(text, ConsoleLineType::Error);
		if (ShowErrors)
			MessageBoxA(NULL, ("Error: FixMissingSounds: " + text).c_str(), "EchoPatch", MB_ICONERROR);
	}

	enum class StereoCheckState { Pending, Applied, Failed };
	inline std::atomic<StereoCheckState> g_stereoCheckState{ StereoCheckState::Pending };
	inline std::mutex g_stereoCheckMutex;
	inline std::string g_stereoCheckStatus = "stereo check not patched yet";

	inline std::string GetStereoCheckStatus()
	{
		std::lock_guard<std::mutex> lock(g_stereoCheckMutex);
		return g_stereoCheckStatus;
	}

	enum class SoundEvent
	{
		StereoConverted,
		MonoCreateFailed,
		HwRetried,
		HwRetryFailed,
		DuplicateWrapped,
		WrappedLock,
		WriteCursorLock,
		UncommonFormat,
		Count
	};

	struct SoundEventInfo
	{
		const char* name;
		ConsoleLineType type;
	};

	inline constexpr SoundEventInfo g_soundEventInfo[] =
	{
		{ "stereo 3D sounds converted to mono", ConsoleLineType::Info },
		{ "mono 3D buffers failed to create", ConsoleLineType::Warning },
		{ "hardware buffers retried in software", ConsoleLineType::Info },
		{ "software retries failed", ConsoleLineType::Warning },
		{ "converted buffers duplicated", ConsoleLineType::Info },
		{ "wrap-around locks on converted buffers", ConsoleLineType::Info },
		{ "write cursor locks on converted buffers", ConsoleLineType::Info },
		{ "converted buffers in a format other than 16-bit PCM", ConsoleLineType::Info },
	};

	inline std::atomic<unsigned> g_soundEventCounts[static_cast<size_t>(SoundEvent::Count)];

	// The first few occurrences are logged in full, then only totals at growing intervals,
	// so a busy session can't flood the console buffer. Totals repeat the patch status, which
	// keeps it in the log file even if the first message came before the file was opened.
	inline void LogSoundEvent(SoundEvent event, const std::string& detail)
	{
		if (!ConsoleEnabled || DebugLevel == 0)
			return;

		const size_t index = static_cast<size_t>(event);
		const unsigned count = ++g_soundEventCounts[index];
		const SoundEventInfo& info = g_soundEventInfo[index];

		constexpr unsigned FullDetailCount = 5;
		if (count <= FullDetailCount)
		{
			LogMissingSounds(detail + " (" + std::to_string(count) + ")", info.type);
			if (count == FullDetailCount)
				LogMissingSounds("further " + std::string(info.name) + " are only counted", info.type);
			return;
		}

		const bool milestone = count == 10 || count == 25 || count == 50 || count == 100 || count == 250 || count == 500 || count % 1000 == 0;
		if (milestone)
			LogMissingSounds(std::to_string(count) + " " + info.name + " so far, " + GetStereoCheckStatus(), info.type);
	}

	// =======================
	// Stereo to mono 3D buffer wrapper
	// =======================

	// Local copies so no extra headers or libraries are needed
	const GUID SUBTYPE_PCM = { 0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71} };
	const GUID SUBTYPE_IEEE_FLOAT = { 0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71} };
	constexpr DWORD CHANNEL_FRONT_CENTER = 0x4;

	const GUID IID_StereoTo3DBufferProxy = { 0x6c1f6a5e, 0x2b7d, 0x4b8e, {0x9a, 0x41, 0x3e, 0x0d, 0x52, 0x7c, 0x18, 0xa4} };

	inline bool IsBufferInterface(REFIID riid)
	{
		return riid == IID_IUnknown || riid == IID_IDirectSoundBuffer || riid == IID_IDirectSoundBuffer8 || riid == IID_StereoTo3DBufferProxy;
	}

	// DirectSound8 does not allow multi-channel 3D buffers, and DSOAL enforces this when hardware
	// mixing is enabled. This wrapper exposes a stereo buffer to the game while the real buffer is a
	// mono 3D buffer. Each Lock hands out a temporary stereo region filled from the mono data, and
	// Unlock downmixes it back, so no stereo copy is kept between locks. Positions and sizes are
	// scaled between the two layouts. The 3D and EAX interfaces are wrapped as well, so asking them
	// for the buffer always returns this wrapper rather than the real mono buffer.
	class StereoTo3DBufferProxy : public IDirectSoundBuffer8
	{
	private:
		struct PendingLock
		{
			DWORD offset;
			DWORD bytes1;
			DWORD bytes2;
			std::vector<BYTE> data;
		};

		IDirectSoundBuffer8* m_pReal;
		volatile LONG m_refCount;
		WAVEFORMATEXTENSIBLE m_stereoFormat{};
		DWORD m_stereoBytes;
		WORD m_bytesPerSample;
		bool m_isFloat;
		std::vector<PendingLock> m_locks;
		std::mutex m_mutex;

		DWORD FrameBytes() const { return m_bytesPerSample * 2u; }

		// Stereo byte offset <-> mono byte offset (one stereo frame is twice the size of a mono frame)
		DWORD ToMono(DWORD stereoOffset) const { return (stereoOffset / FrameBytes()) * m_bytesPerSample; }
		DWORD ToStereo(DWORD monoOffset) const { return (monoOffset / m_bytesPerSample) * FrameBytes(); }

		void DownmixFrames(const BYTE* src, BYTE* dst, DWORD frames) const
		{
			for (DWORD i = 0; i < frames; ++i)
			{
				if (m_isFloat)
				{
					const float* s = reinterpret_cast<const float*>(src) + i * 2;
					reinterpret_cast<float*>(dst)[i] = (s[0] + s[1]) * 0.5f;
				}
				else if (m_bytesPerSample == 2)
				{
					const int16_t* s = reinterpret_cast<const int16_t*>(src) + i * 2;
					reinterpret_cast<int16_t*>(dst)[i] = static_cast<int16_t>((static_cast<int>(s[0]) + s[1]) / 2);
				}
				else // 8-bit unsigned
				{
					const uint8_t* s = src + i * 2;
					dst[i] = static_cast<uint8_t>((static_cast<int>(s[0]) + s[1]) / 2);
				}
			}
		}

		void UpmixFrames(const BYTE* src, BYTE* dst, DWORD frames) const
		{
			for (DWORD i = 0; i < frames; ++i)
			{
				const BYTE* s = src + i * m_bytesPerSample;
				memcpy(dst + i * FrameBytes(), s, m_bytesPerSample);
				memcpy(dst + i * FrameBytes() + m_bytesPerSample, s, m_bytesPerSample);
			}
		}

		// Copies a mono region of the real buffer into stereo memory, so the game reads sensible data
		void ReadRegion(DWORD stereoOffset, DWORD stereoBytes, BYTE* dst)
		{
			const DWORD frames = stereoBytes / FrameBytes();
			if (frames == 0)
				return;

			void* p1 = nullptr; void* p2 = nullptr;
			DWORD b1 = 0, b2 = 0;
			if (FAILED(m_pReal->Lock(ToMono(stereoOffset), frames * m_bytesPerSample, &p1, &b1, &p2, &b2, 0)))
				return;

			UpmixFrames(static_cast<const BYTE*>(p1), dst, b1 / m_bytesPerSample);
			if (p2 && b2)
				UpmixFrames(static_cast<const BYTE*>(p2), dst + (b1 / m_bytesPerSample) * FrameBytes(), b2 / m_bytesPerSample);

			m_pReal->Unlock(p1, b1, p2, b2);
		}

		// Downmixes stereo memory into a mono region of the real buffer
		void WriteRegion(DWORD stereoOffset, DWORD stereoBytes, const BYTE* src)
		{
			const DWORD frames = stereoBytes / FrameBytes();
			if (frames == 0)
				return;

			void* p1 = nullptr; void* p2 = nullptr;
			DWORD b1 = 0, b2 = 0;
			if (FAILED(m_pReal->Lock(ToMono(stereoOffset), frames * m_bytesPerSample, &p1, &b1, &p2, &b2, 0)))
				return;

			DownmixFrames(src, static_cast<BYTE*>(p1), b1 / m_bytesPerSample);
			if (p2 && b2)
				DownmixFrames(src + (b1 / m_bytesPerSample) * FrameBytes(), static_cast<BYTE*>(p2), b2 / m_bytesPerSample);

			m_pReal->Unlock(p1, b1, p2, b2);
		}

	public:
		StereoTo3DBufferProxy(IDirectSoundBuffer8* pReal, const WAVEFORMATEX* stereoFormat, DWORD stereoBytes, bool isFloat)
			: m_pReal(pReal), m_refCount(1), m_stereoBytes(stereoBytes), m_bytesPerSample(stereoFormat->wBitsPerSample / 8), m_isFloat(isFloat)
		{
			const size_t formatSize = (stereoFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) ? sizeof(WAVEFORMATEXTENSIBLE) : sizeof(WAVEFORMATEX);
			memcpy(&m_stereoFormat, stereoFormat, formatSize);
			if (stereoFormat->wFormatTag != WAVE_FORMAT_EXTENSIBLE)
				m_stereoFormat.Format.cbSize = 0;
		}

		~StereoTo3DBufferProxy()
		{
			if (m_pReal)
				m_pReal->Release();
		}

		IDirectSoundBuffer8* GetReal() const { return m_pReal; }
		const WAVEFORMATEX* GetStereoFormat() const { return &m_stereoFormat.Format; }
		DWORD GetStereoBytes() const { return m_stereoBytes; }
		bool IsFloat() const { return m_isFloat; }

		// IUnknown
		STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppv);

		STDMETHOD_(ULONG, AddRef)()
		{
			return InterlockedIncrement(&m_refCount);
		}

		STDMETHOD_(ULONG, Release)()
		{
			ULONG ref = InterlockedDecrement(&m_refCount);
			if (ref == 0)
				delete this;
			return ref;
		}

		// IDirectSoundBuffer
		STDMETHOD(GetCaps)(LPDSBCAPS pDSBufferCaps)
		{
			HRESULT hr = m_pReal->GetCaps(pDSBufferCaps);
			if (SUCCEEDED(hr))
				pDSBufferCaps->dwBufferBytes = m_stereoBytes;
			return hr;
		}

		STDMETHOD(GetCurrentPosition)(LPDWORD pdwCurrentPlayCursor, LPDWORD pdwCurrentWriteCursor)
		{
			HRESULT hr = m_pReal->GetCurrentPosition(pdwCurrentPlayCursor, pdwCurrentWriteCursor);
			if (SUCCEEDED(hr))
			{
				if (pdwCurrentPlayCursor) *pdwCurrentPlayCursor = ToStereo(*pdwCurrentPlayCursor);
				if (pdwCurrentWriteCursor) *pdwCurrentWriteCursor = ToStereo(*pdwCurrentWriteCursor);
			}
			return hr;
		}

		STDMETHOD(GetFormat)(LPWAVEFORMATEX pwfxFormat, DWORD dwSizeAllocated, LPDWORD pdwSizeWritten)
		{
			const DWORD size = sizeof(WAVEFORMATEX) + m_stereoFormat.Format.cbSize;
			if (pdwSizeWritten) *pdwSizeWritten = size;
			if (!pwfxFormat) return pdwSizeWritten ? DS_OK : DSERR_INVALIDPARAM;
			memcpy(pwfxFormat, &m_stereoFormat, (std::min)(size, dwSizeAllocated));
			return DS_OK;
		}

		STDMETHOD(GetVolume)(LPLONG plVolume) { return m_pReal->GetVolume(plVolume); }
		STDMETHOD(GetPan)(LPLONG plPan) { return m_pReal->GetPan(plPan); }
		STDMETHOD(GetFrequency)(LPDWORD pdwFrequency) { return m_pReal->GetFrequency(pdwFrequency); }
		STDMETHOD(GetStatus)(LPDWORD pdwStatus) { return m_pReal->GetStatus(pdwStatus); }
		STDMETHOD(Initialize)(LPDIRECTSOUND, LPCDSBUFFERDESC) { return DSERR_ALREADYINITIALIZED; }

		STDMETHOD(Lock)(DWORD dwOffset, DWORD dwBytes, LPVOID* ppvAudioPtr1, LPDWORD pdwAudioBytes1, LPVOID* ppvAudioPtr2, LPDWORD pdwAudioBytes2, DWORD dwFlags)
		{
			if (!ppvAudioPtr1 || !pdwAudioBytes1)
				return DSERR_INVALIDPARAM;

			if (dwFlags & DSBLOCK_FROMWRITECURSOR)
			{
				DWORD play = 0, write = 0;
				HRESULT hr = GetCurrentPosition(&play, &write);
				if (FAILED(hr)) return hr;
				dwOffset = write;
				LogSoundEvent(SoundEvent::WriteCursorLock, "write cursor lock on a converted buffer at " + std::to_string(write));
			}
			if (dwFlags & DSBLOCK_ENTIREBUFFER)
			{
				dwOffset = 0;
				dwBytes = m_stereoBytes;
			}

			dwOffset -= dwOffset % FrameBytes();
			dwBytes -= dwBytes % FrameBytes();
			if (dwOffset >= m_stereoBytes || dwBytes == 0 || dwBytes > m_stereoBytes)
				return DSERR_INVALIDPARAM;

			// Same ring buffer semantics as DirectSound: wrap around into a second region
			PendingLock pending;
			pending.offset = dwOffset;
			pending.bytes1 = (std::min)(dwBytes, m_stereoBytes - dwOffset);
			pending.bytes2 = dwBytes - pending.bytes1;
			pending.data.resize(dwBytes);

			if (pending.bytes2)
				LogSoundEvent(SoundEvent::WrappedLock, "wrap-around lock on a converted buffer, " + std::to_string(pending.bytes1) + " + " + std::to_string(pending.bytes2) + " bytes");

			ReadRegion(pending.offset, pending.bytes1, pending.data.data());
			if (pending.bytes2)
				ReadRegion(0, pending.bytes2, pending.data.data() + pending.bytes1);

			*ppvAudioPtr1 = pending.data.data();
			*pdwAudioBytes1 = pending.bytes1;
			if (ppvAudioPtr2) *ppvAudioPtr2 = pending.bytes2 ? pending.data.data() + pending.bytes1 : nullptr;
			if (pdwAudioBytes2) *pdwAudioBytes2 = pending.bytes2;

			std::lock_guard<std::mutex> lock(m_mutex);
			m_locks.push_back(std::move(pending));
			return DS_OK;
		}

		STDMETHOD(Play)(DWORD dwReserved1, DWORD dwPriority, DWORD dwFlags) { return m_pReal->Play(dwReserved1, dwPriority, dwFlags); }

		STDMETHOD(SetCurrentPosition)(DWORD dwNewPosition)
		{
			return m_pReal->SetCurrentPosition(ToMono((std::min)(dwNewPosition, m_stereoBytes)));
		}

		STDMETHOD(SetFormat)(LPCWAVEFORMATEX) { return DSERR_INVALIDCALL; }
		STDMETHOD(SetVolume)(LONG lVolume) { return m_pReal->SetVolume(lVolume); }
		STDMETHOD(SetPan)(LONG lPan) { return m_pReal->SetPan(lPan); }
		STDMETHOD(SetFrequency)(DWORD dwFrequency) { return m_pReal->SetFrequency(dwFrequency); }
		STDMETHOD(Stop)() { return m_pReal->Stop(); }

		STDMETHOD(Unlock)(LPVOID pvAudioPtr1, DWORD dwAudioBytes1, LPVOID pvAudioPtr2, DWORD dwAudioBytes2)
		{
			PendingLock pending;
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				auto it = std::find_if(m_locks.begin(), m_locks.end(), [&](const PendingLock& p) { return p.data.data() == pvAudioPtr1; });
				if (it == m_locks.end())
					return DSERR_INVALIDPARAM;
				pending = std::move(*it);
				m_locks.erase(it);
			}

			WriteRegion(pending.offset, (std::min)(dwAudioBytes1, pending.bytes1), pending.data.data());
			if (pvAudioPtr2 && dwAudioBytes2 && pending.bytes2)
				WriteRegion(0, (std::min)(dwAudioBytes2, pending.bytes2), pending.data.data() + pending.bytes1);

			return DS_OK;
		}

		STDMETHOD(Restore)() { return m_pReal->Restore(); }

		// IDirectSoundBuffer8
		STDMETHOD(SetFX)(DWORD dwEffectsCount, LPDSEFFECTDESC pDSFXDesc, LPDWORD pdwResultCodes) { return m_pReal->SetFX(dwEffectsCount, pDSFXDesc, pdwResultCodes); }
		STDMETHOD(AcquireResources)(DWORD dwFlags, DWORD dwEffectsCount, LPDWORD pdwResultCodes) { return m_pReal->AcquireResources(dwFlags, dwEffectsCount, pdwResultCodes); }
		STDMETHOD(GetObjectInPath)(REFGUID rguidObject, DWORD dwIndex, REFGUID rguidInterface, LPVOID* ppObject) { return m_pReal->GetObjectInPath(rguidObject, dwIndex, rguidInterface, ppObject); }
	};

	// 3D interface of a converted buffer: forwards to the real mono buffer, but resolves buffer
	// interfaces through the wrapper so the real mono buffer is never handed to the game
	class Converted3DBufferProxy : public IDirectSound3DBuffer
	{
	private:
		StereoTo3DBufferProxy* m_pParent;
		IDirectSound3DBuffer* m_pReal;
		volatile LONG m_refCount;

	public:
		Converted3DBufferProxy(StereoTo3DBufferProxy* pParent, IDirectSound3DBuffer* pReal) : m_pParent(pParent), m_pReal(pReal), m_refCount(1) { m_pParent->AddRef(); }
		~Converted3DBufferProxy() { m_pReal->Release(); m_pParent->Release(); }

		STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppv)
		{
			if (!ppv) return E_POINTER;
			if (riid == IID_IDirectSound3DBuffer)
			{
				*ppv = static_cast<IDirectSound3DBuffer*>(this);
				AddRef();
				return S_OK;
			}
			return m_pParent->QueryInterface(riid, ppv);
		}
		STDMETHOD_(ULONG, AddRef)() { return InterlockedIncrement(&m_refCount); }
		STDMETHOD_(ULONG, Release)()
		{
			ULONG ref = InterlockedDecrement(&m_refCount);
			if (ref == 0)
				delete this;
			return ref;
		}

		STDMETHOD(GetAllParameters)(LPDS3DBUFFER pDs3dBuffer) { return m_pReal->GetAllParameters(pDs3dBuffer); }
		STDMETHOD(GetConeAngles)(LPDWORD pdwInsideConeAngle, LPDWORD pdwOutsideConeAngle) { return m_pReal->GetConeAngles(pdwInsideConeAngle, pdwOutsideConeAngle); }
		STDMETHOD(GetConeOrientation)(D3DVECTOR* pvOrientation) { return m_pReal->GetConeOrientation(pvOrientation); }
		STDMETHOD(GetConeOutsideVolume)(LPLONG plConeOutsideVolume) { return m_pReal->GetConeOutsideVolume(plConeOutsideVolume); }
		STDMETHOD(GetMaxDistance)(D3DVALUE* pflMaxDistance) { return m_pReal->GetMaxDistance(pflMaxDistance); }
		STDMETHOD(GetMinDistance)(D3DVALUE* pflMinDistance) { return m_pReal->GetMinDistance(pflMinDistance); }
		STDMETHOD(GetMode)(LPDWORD pdwMode) { return m_pReal->GetMode(pdwMode); }
		STDMETHOD(GetPosition)(D3DVECTOR* pvPosition) { return m_pReal->GetPosition(pvPosition); }
		STDMETHOD(GetVelocity)(D3DVECTOR* pvVelocity) { return m_pReal->GetVelocity(pvVelocity); }
		STDMETHOD(SetAllParameters)(LPCDS3DBUFFER pcDs3dBuffer, DWORD dwApply) { return m_pReal->SetAllParameters(pcDs3dBuffer, dwApply); }
		STDMETHOD(SetConeAngles)(DWORD dwInsideConeAngle, DWORD dwOutsideConeAngle, DWORD dwApply) { return m_pReal->SetConeAngles(dwInsideConeAngle, dwOutsideConeAngle, dwApply); }
		STDMETHOD(SetConeOrientation)(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD dwApply) { return m_pReal->SetConeOrientation(x, y, z, dwApply); }
		STDMETHOD(SetConeOutsideVolume)(LONG lConeOutsideVolume, DWORD dwApply) { return m_pReal->SetConeOutsideVolume(lConeOutsideVolume, dwApply); }
		STDMETHOD(SetMaxDistance)(D3DVALUE flMaxDistance, DWORD dwApply) { return m_pReal->SetMaxDistance(flMaxDistance, dwApply); }
		STDMETHOD(SetMinDistance)(D3DVALUE flMinDistance, DWORD dwApply) { return m_pReal->SetMinDistance(flMinDistance, dwApply); }
		STDMETHOD(SetMode)(DWORD dwMode, DWORD dwApply) { return m_pReal->SetMode(dwMode, dwApply); }
		STDMETHOD(SetPosition)(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD dwApply) { return m_pReal->SetPosition(x, y, z, dwApply); }
		STDMETHOD(SetVelocity)(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD dwApply) { return m_pReal->SetVelocity(x, y, z, dwApply); }
	};

	// EAX property interface of a converted buffer, same routing as Converted3DBufferProxy
	class ConvertedPropertySetProxy : public IKsPropertySet
	{
	private:
		StereoTo3DBufferProxy* m_pParent;
		IKsPropertySet* m_pReal;
		volatile LONG m_refCount;

	public:
		ConvertedPropertySetProxy(StereoTo3DBufferProxy* pParent, IKsPropertySet* pReal) : m_pParent(pParent), m_pReal(pReal), m_refCount(1) { m_pParent->AddRef(); }
		~ConvertedPropertySetProxy() { m_pReal->Release(); m_pParent->Release(); }

		STDMETHOD(QueryInterface)(REFIID riid, LPVOID* ppv)
		{
			if (!ppv) return E_POINTER;
			if (riid == IID_IKsPropertySet)
			{
				*ppv = static_cast<IKsPropertySet*>(this);
				AddRef();
				return S_OK;
			}
			return m_pParent->QueryInterface(riid, ppv);
		}
		STDMETHOD_(ULONG, AddRef)() { return InterlockedIncrement(&m_refCount); }
		STDMETHOD_(ULONG, Release)()
		{
			ULONG ref = InterlockedDecrement(&m_refCount);
			if (ref == 0)
				delete this;
			return ref;
		}

		STDMETHOD(Get)(REFGUID rguidPropSet, ULONG ulId, LPVOID pInstanceData, ULONG ulInstanceLength, LPVOID pPropertyData, ULONG ulDataLength, PULONG pulBytesReturned)
		{
			return m_pReal->Get(rguidPropSet, ulId, pInstanceData, ulInstanceLength, pPropertyData, ulDataLength, pulBytesReturned);
		}
		STDMETHOD(Set)(REFGUID rguidPropSet, ULONG ulId, LPVOID pInstanceData, ULONG ulInstanceLength, LPVOID pPropertyData, ULONG ulDataLength)
		{
			return m_pReal->Set(rguidPropSet, ulId, pInstanceData, ulInstanceLength, pPropertyData, ulDataLength);
		}
		STDMETHOD(QuerySupport)(REFGUID rguidPropSet, ULONG ulId, PULONG pulTypeSupport)
		{
			return m_pReal->QuerySupport(rguidPropSet, ulId, pulTypeSupport);
		}
	};

	inline HRESULT STDMETHODCALLTYPE StereoTo3DBufferProxy::QueryInterface(REFIID riid, LPVOID* ppv)
	{
		if (!ppv) return E_POINTER;
		*ppv = nullptr;

		if (IsBufferInterface(riid))
		{
			*ppv = static_cast<IDirectSoundBuffer8*>(this);
			AddRef();
			return S_OK;
		}

		if (riid == IID_IDirectSound3DBuffer)
		{
			IDirectSound3DBuffer* pReal3D = nullptr;
			HRESULT hr = m_pReal->QueryInterface(riid, reinterpret_cast<void**>(&pReal3D));
			if (FAILED(hr)) return hr;
			auto proxy = new (std::nothrow) Converted3DBufferProxy(this, pReal3D);
			if (!proxy) { pReal3D->Release(); return E_OUTOFMEMORY; }
			*ppv = static_cast<IDirectSound3DBuffer*>(proxy);
			return S_OK;
		}

		if (riid == IID_IKsPropertySet)
		{
			IKsPropertySet* pRealProps = nullptr;
			HRESULT hr = m_pReal->QueryInterface(riid, reinterpret_cast<void**>(&pRealProps));
			if (FAILED(hr)) return hr;
			auto proxy = new (std::nothrow) ConvertedPropertySetProxy(this, pRealProps);
			if (!proxy) { pRealProps->Release(); return E_OUTOFMEMORY; }
			*ppv = static_cast<IKsPropertySet*>(proxy);
			return S_OK;
		}

		return m_pReal->QueryInterface(riid, ppv);
	}

	// Returns true for uncompressed 8/16-bit PCM or 32-bit float stereo, the only layouts the downmix handles
	inline bool IsDownmixableStereo(const WAVEFORMATEX* wfx, bool& isFloat)
	{
		if (!wfx || wfx->nChannels != 2)
			return false;

		WORD tag = wfx->wFormatTag;
		if (tag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22)
		{
			const GUID& sub = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx)->SubFormat;
			if (sub == SUBTYPE_PCM) tag = WAVE_FORMAT_PCM;
			else if (sub == SUBTYPE_IEEE_FLOAT) tag = WAVE_FORMAT_IEEE_FLOAT;
		}

		isFloat = (tag == WAVE_FORMAT_IEEE_FLOAT && wfx->wBitsPerSample == 32);
		const bool isPcm = (tag == WAVE_FORMAT_PCM && (wfx->wBitsPerSample == 8 || wfx->wBitsPerSample == 16));
		return isFloat || isPcm;
	}


	// =======================
	// Buffer creation
	// =======================

	// Creates a buffer, retrying without LOCHARDWARE if the hardware voice pool is full
	inline HRESULT CreateWithHwFallback(IDirectSound8* pDS, const DSBUFFERDESC& desc, LPDIRECTSOUNDBUFFER* ppDSBuffer, LPUNKNOWN pUnkOuter)
	{
		HRESULT hr = pDS->CreateSoundBuffer(&desc, ppDSBuffer, pUnkOuter);
		if (hr == DSERR_ALLOCATED && (desc.dwFlags & DSBCAPS_LOCHARDWARE))
		{
			DSBUFFERDESC retry = desc;
			retry.dwFlags &= ~DSBCAPS_LOCHARDWARE;
			hr = pDS->CreateSoundBuffer(&retry, ppDSBuffer, pUnkOuter);

			if (SUCCEEDED(hr))
				LogSoundEvent(SoundEvent::HwRetried, "hardware voices exhausted, " + std::to_string(desc.dwBufferBytes) + " byte buffer created in software");
			else
				LogSoundEvent(SoundEvent::HwRetryFailed, "hardware voices exhausted and the software retry of a " + std::to_string(desc.dwBufferBytes) + " byte buffer failed");
		}
		return hr;
	}

	void EnsureStereoCheckPatched(bool finalAttempt);

	HRESULT FixedCreateSoundBuffer(IDirectSound8* pDS, LPCDSBUFFERDESC pcDSBufferDesc, LPDIRECTSOUNDBUFFER* ppDSBuffer, LPUNKNOWN pUnkOuter)
	{
		// The game creates its buffers through SndDrv.dll, so it is loaded by now
		EnsureStereoCheckPatched(true);

		if (!pcDSBufferDesc || !ppDSBuffer)
			return pDS->CreateSoundBuffer(pcDSBufferDesc, ppDSBuffer, pUnkOuter);

		// Copy only dwSize bytes, the game may pass the smaller DirectX 3 DSBUFFERDESC1 layout
		DSBUFFERDESC desc{};
		memcpy(&desc, pcDSBufferDesc, (std::min)(static_cast<size_t>(pcDSBufferDesc->dwSize), sizeof(desc)));
		desc.dwSize = (std::max)(static_cast<DWORD>(sizeof(DSBUFFERDESC1)), (std::min)(pcDSBufferDesc->dwSize, static_cast<DWORD>(sizeof(desc))));

		HRESULT hr = CreateWithHwFallback(pDS, desc, ppDSBuffer, pUnkOuter);

		// Stereo 3D buffers are rejected on DirectSound8 devices: create a mono 3D buffer and downmix into it
		bool isFloat = false;
		if (hr != DSERR_INVALIDPARAM || !(desc.dwFlags & DSBCAPS_CTRL3D) || !IsDownmixableStereo(desc.lpwfxFormat, isFloat))
			return hr;

		const WAVEFORMATEX* stereo = desc.lpwfxFormat;

		WAVEFORMATEXTENSIBLE mono{};
		const size_t formatSize = (stereo->wFormatTag == WAVE_FORMAT_EXTENSIBLE) ? sizeof(WAVEFORMATEXTENSIBLE) : sizeof(WAVEFORMATEX);
		memcpy(&mono, stereo, formatSize);
		if (stereo->wFormatTag != WAVE_FORMAT_EXTENSIBLE)
			mono.Format.cbSize = 0;
		else
			mono.dwChannelMask = CHANNEL_FRONT_CENTER;
		mono.Format.nChannels = 1;
		mono.Format.nBlockAlign = stereo->nBlockAlign / 2;
		mono.Format.nAvgBytesPerSec = mono.Format.nSamplesPerSec * mono.Format.nBlockAlign;

		const DWORD stereoBytes = (desc.dwBufferBytes / stereo->nBlockAlign) * stereo->nBlockAlign;

		DSBUFFERDESC monoDesc = desc;
		monoDesc.lpwfxFormat = &mono.Format;
		monoDesc.dwBufferBytes = (stereoBytes / stereo->nBlockAlign) * mono.Format.nBlockAlign;

		const std::string formatText = std::to_string(stereo->nSamplesPerSec) + " Hz " + std::to_string(stereo->wBitsPerSample) + "-bit" + (isFloat ? " float" : "");

		LPDIRECTSOUNDBUFFER pMono = nullptr;
		hr = CreateWithHwFallback(pDS, monoDesc, &pMono, pUnkOuter);
		IDirectSoundBuffer8* pMono8 = nullptr;
		if (SUCCEEDED(hr))
		{
			hr = pMono->QueryInterface(IID_IDirectSoundBuffer8, reinterpret_cast<void**>(&pMono8));
			pMono->Release();
		}
		if (FAILED(hr))
		{
			char code[16];
			sprintf_s(code, "0x%08X", static_cast<unsigned>(hr));
			LogSoundEvent(SoundEvent::MonoCreateFailed, "mono 3D buffer for a " + std::to_string(stereoBytes) + " byte stereo sound (" + formatText + ") failed: " + code);
			return hr;
		}

		auto proxy = new (std::nothrow) StereoTo3DBufferProxy(pMono8, stereo, stereoBytes, isFloat);
		if (!proxy)
		{
			pMono8->Release();
			return DSERR_OUTOFMEMORY;
		}

		*ppDSBuffer = reinterpret_cast<LPDIRECTSOUNDBUFFER>(static_cast<IDirectSoundBuffer8*>(proxy));
		LogSoundEvent(SoundEvent::StereoConverted, "stereo 3D sound converted to mono, " + std::to_string(stereoBytes) + " -> " +
			std::to_string(monoDesc.dwBufferBytes) + " bytes, " + formatText);
		if (isFloat || stereo->wBitsPerSample != 16)
			LogSoundEvent(SoundEvent::UncommonFormat, "converted buffer uses " + formatText);
		return DS_OK;
	}

	HRESULT FixedDuplicateSoundBuffer(IDirectSound8* pDS, LPDIRECTSOUNDBUFFER pDSBufferOriginal, LPDIRECTSOUNDBUFFER* ppDSBufferDuplicate)
	{
		// A downmix wrapper must not reach the real device: duplicate its mono buffer and wrap the copy.
		// Duplicates share the real audio data, so the new wrapper starts with the same sound.
		StereoTo3DBufferProxy* pWrapper = nullptr;
		if (!pDSBufferOriginal || !ppDSBufferDuplicate ||
			FAILED(pDSBufferOriginal->QueryInterface(IID_StereoTo3DBufferProxy, reinterpret_cast<void**>(&pWrapper))))
		{
			return pDS->DuplicateSoundBuffer(pDSBufferOriginal, ppDSBufferDuplicate);
		}

		LPDIRECTSOUNDBUFFER pDup = nullptr;
		HRESULT hr = pDS->DuplicateSoundBuffer(reinterpret_cast<LPDIRECTSOUNDBUFFER>(pWrapper->GetReal()), &pDup);
		if (SUCCEEDED(hr))
		{
			IDirectSoundBuffer8* pDup8 = nullptr;
			hr = pDup->QueryInterface(IID_IDirectSoundBuffer8, reinterpret_cast<void**>(&pDup8));
			pDup->Release();
			if (SUCCEEDED(hr))
			{
				auto proxy = new (std::nothrow) StereoTo3DBufferProxy(pDup8, pWrapper->GetStereoFormat(), pWrapper->GetStereoBytes(), pWrapper->IsFloat());
				if (proxy)
					*ppDSBufferDuplicate = reinterpret_cast<LPDIRECTSOUNDBUFFER>(static_cast<IDirectSoundBuffer8*>(proxy));
				else
				{
					pDup8->Release();
					hr = DSERR_OUTOFMEMORY;
				}
			}
		}

		char code[16];
		sprintf_s(code, "0x%08X", static_cast<unsigned>(hr));
		LogSoundEvent(SoundEvent::DuplicateWrapped, "converted buffer duplicated: " + std::string(SUCCEEDED(hr) ? "ok" : code));

		pWrapper->Release();
		return hr;
	}

	// =======================
	// SndDrv.dll stereo check
	// =======================

	// cmp r16, imm8 (66 83 F8+r imm) or cmp ax, imm16 (66 3D imm 00) within [start, end)
	inline bool HasFormatTagCompare(const BYTE* start, const BYTE* end, BYTE imm)
	{
		for (const BYTE* p = start; p + 4 <= end; ++p)
		{
			if (p[0] == 0x66 && p[1] == 0x83 && p[2] >= 0xF8 && p[3] == imm)
				return true;
			if (p[0] == 0x66 && p[1] == 0x3D && p[2] == imm && p[3] == 0x00)
				return true;
		}
		return false;
	}

	// Finds the stereo check of Init3DSampleFromFile by its structure: "cmp word ptr [reg+2], 1" (nChannels),
	// a "ja" shortly after, then the format tag checks for PCM (1), ADPCM (0x11) and MP3 (0x55).
	// Returns the number of matches; jumpAddr/jumpSize describe the jump when there is exactly one.
	inline int FindStereoCheckByStructure(HMODULE hModule, DWORD& jumpAddr, DWORD& jumpSize)
	{
		auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hModule);
		auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(hModule) + dos->e_lfanew);
		auto section = IMAGE_FIRST_SECTION(nt);

		int matches = 0;
		for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
		{
			if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE))
				continue;

			const BYTE* begin = reinterpret_cast<const BYTE*>(hModule) + section->VirtualAddress;
			const BYTE* end = begin + section->Misc.VirtualSize;

			for (const BYTE* p = begin; p + 0x240 < end; ++p)
			{
				// 66 83 7x 02 01 : cmp word ptr [reg+2], 1 (7C would need a SIB byte)
				if (p[0] != 0x66 || p[1] != 0x83 || p[2] < 0x78 || p[2] > 0x7F || p[2] == 0x7C || p[3] != 0x02 || p[4] != 0x01)
					continue;

				const BYTE* jump = nullptr;
				DWORD size = 0;
				for (const BYTE* q = p + 5; q < p + 0x20; ++q)
				{
					if (q[0] == 0x0F && q[1] == 0x87) { jump = q; size = 6; break; }
					if (q[0] == 0x77) { jump = q; size = 2; break; }
				}
				if (!jump)
					continue;

				const BYTE* after = jump + size;
				if (!HasFormatTagCompare(after, after + 0x20, 0x01) ||
					!HasFormatTagCompare(after, after + 0x80, 0x11) ||
					!HasFormatTagCompare(after, after + 0x200, 0x55))
					continue;

				if (++matches == 1)
				{
					jumpAddr = reinterpret_cast<DWORD>(jump);
					jumpSize = size;
				}
			}
		}
		return matches;
	}

	// Removes the "nChannels > 1 -> fail" jump in CDx8SoundSys::Init3DSampleFromFile.
	// Tried on every device creation and, as the final attempt, on every buffer creation, so a sound
	// driver loaded after the first device probe is still patched. Errors are reported once.
	void EnsureStereoCheckPatched(bool finalAttempt)
	{
		if (g_stereoCheckState.load() != StereoCheckState::Pending)
			return;

		std::string message;
		std::string error;
		{
			std::lock_guard<std::mutex> lock(g_stereoCheckMutex);
			if (g_stereoCheckState.load() != StereoCheckState::Pending)
				return;

			HMODULE hSndDrv = GetModuleHandleA("SndDrv.dll");
			if (!hSndDrv)
			{
				if (!finalAttempt)
					return;
				g_stereoCheckStatus = "stereo check not patched, SndDrv.dll is not loaded";
				g_stereoCheckState = StereoCheckState::Failed;
				error = "SndDrv.dll is not loaded, stereo 3D sounds will stay silent";
			}
			else
			{
				struct StereoCheck { const char* signature; DWORD jumpOffset; const char* build; };
				static const StereoCheck checks[] =
				{
					// cmp word ptr [eax+2], 1 / push ebx / push edi / lea edi, [eax+2] / mov ebx, eax / ja fail
					{ "66 83 78 02 01 53 57 8D 78 02 8B D8 0F 87 ?? ?? ?? ??", 12, "Extraction Point / Perseus Mandate" },
					// cmp word ptr [eax+2], 1 / push esi / push edi / lea edi, [eax+2] / mov esi, eax / mov [esp+34h], esi / ja fail
					{ "66 83 78 02 01 56 57 8D 78 02 8B F0 89 74 24 34 0F 87 ?? ?? ?? ??", 16, "F.E.A.R." },
				};

				DWORD jumpAddr = 0;
				DWORD jumpSize = 0;
				const char* method = nullptr;

				for (const auto& check : checks)
				{
					DWORD addr = MemoryHelper::FindSignatureAddress(hSndDrv, check.signature, -1);
					if (addr)
					{
						jumpAddr = addr + check.jumpOffset;
						jumpSize = 6;
						method = check.build;
						break;
					}
				}

				int matches = 1;
				if (!jumpAddr)
				{
					matches = FindStereoCheckByStructure(hSndDrv, jumpAddr, jumpSize);
					method = "structure search";
				}

				// The jump must still be there: 0F 87 rel32 or 77 rel8
				const BYTE* jump = reinterpret_cast<const BYTE*>(jumpAddr);
				const bool validJump = matches == 1 && jumpAddr &&
					((jumpSize == 6 && jump[0] == 0x0F && jump[1] == 0x87) || (jumpSize == 2 && jump[0] == 0x77));

				if (!validJump)
				{
					g_stereoCheckStatus = "stereo check not patched";
					g_stereoCheckState = StereoCheckState::Failed;
					error = "the stereo check in SndDrv.dll was " +
						std::string(matches == 0 ? "not found" : matches > 1 ? "found " + std::to_string(matches) + " times" : "not in the expected form") +
						", stereo 3D sounds will stay silent";
				}
				else
				{
					const BYTE nops[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
					MemoryHelper::WriteMemoryRaw(jumpAddr, nops, jumpSize);

					char offset[16];
					sprintf_s(offset, "0x%X", static_cast<unsigned>(jumpAddr - reinterpret_cast<DWORD>(hSndDrv)));
					g_stereoCheckStatus = "stereo check removed at SndDrv.dll+" + std::string(offset) + " (" + method + ")";
					g_stereoCheckState = StereoCheckState::Applied;
					message = g_stereoCheckStatus;
				}
			}
		}

		// Reported outside the lock, the popup blocks until dismissed
		if (!error.empty())
			ShowMissingSoundsError(error);
		else if (!message.empty())
			LogMissingSounds(message);
	}

	void OnDirectSoundCreated()
	{
		EnsureStereoCheckPatched(false);
	}
}
