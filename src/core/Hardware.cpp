#include "core/Hardware.h"

#include <obs.h>

#include <dxgi.h>
#include <thread>
#include <windows.h>

namespace {
bool hasEncoder(const char *wanted)
{
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); i++) {
		if (strcmp(id, wanted) == 0)
			return true;
	}
	return false;
}

QString firstGpuName()
{
	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))))
		return {};
	QString name;
	IDXGIAdapter1 *adapter = nullptr;
	if (SUCCEEDED(factory->EnumAdapters1(0, &adapter))) {
		DXGI_ADAPTER_DESC1 desc{};
		if (SUCCEEDED(adapter->GetDesc1(&desc)))
			name = QString::fromWCharArray(desc.Description);
		adapter->Release();
	}
	factory->Release();
	return name;
}
} // namespace

Hardware probeHardware()
{
	Hardware hw;
	hw.cores = int(std::max(1u, std::thread::hardware_concurrency()));
	MEMORYSTATUSEX mem{sizeof(mem)};
	if (GlobalMemoryStatusEx(&mem))
		hw.ramGb = double(mem.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
	hw.gpu = firstGpuName();
	hw.nvenc = hasEncoder("obs_nvenc_h264_tex");
	hw.amf = hasEncoder("h264_texture_amf");
	hw.qsv = hasEncoder("obs_qsv11_v2");
	return hw;
}

Profile pickProfile(const Hardware &hw)
{
	if (hw.ramGb < 6.0 || hw.cores <= 2)
		return Profile::Potato;
	if (hw.anyHardwareEncoder() && hw.ramGb >= 15.0 && hw.cores >= 8)
		return Profile::Ultra;
	if (hw.anyHardwareEncoder() || (hw.cores >= 6 && hw.ramGb >= 8.0))
		return Profile::Balanced;
	return Profile::Potato;
}

QString bestEncoder(const Hardware &hw)
{
	// A hardware encoder costs almost no CPU, which is what keeps low-end PCs smooth.
	if (hw.nvenc)
		return "obs_nvenc_h264_tex";
	if (hw.amf)
		return "h264_texture_amf";
	if (hw.qsv)
		return "obs_qsv11_v2";
	return "obs_x264";
}

VideoPlan planFor(Profile profile, const Hardware &hw)
{
	VideoPlan plan;
	plan.encoder = bestEncoder(hw);
	const bool hardware = plan.encoder != "obs_x264";
	switch (profile) {
	case Profile::Potato:
		plan = {1280, 720, 30, 2500, plan.encoder};
		break;
	case Profile::Balanced:
		plan = hardware ? VideoPlan{1920, 1080, 30, 4500, plan.encoder}
				: VideoPlan{1280, 720, 30, 3000, plan.encoder};
		break;
	case Profile::Ultra:
		plan = hardware ? VideoPlan{1920, 1080, 60, 6000, plan.encoder}
				: VideoPlan{1920, 1080, 30, 4500, plan.encoder};
		break;
	}
	return plan;
}

QString encoderLabel(const QString &id)
{
	if (id == "obs_nvenc_h264_tex")
		return "NVIDIA NVENC";
	if (id == "h264_texture_amf")
		return "AMD AMF";
	if (id == "obs_qsv11_v2")
		return "Intel QuickSync";
	return "x264 (CPU)";
}

QString profileName(Profile profile)
{
	switch (profile) {
	case Profile::Potato:
		return "Potato";
	case Profile::Ultra:
		return "Ultra";
	default:
		return "Balanced";
	}
}

QString profileKey(Profile profile)
{
	return profileName(profile).toLower();
}

Profile profileFromKey(const QString &key)
{
	if (key == "potato")
		return Profile::Potato;
	if (key == "ultra")
		return Profile::Ultra;
	return Profile::Balanced;
}
