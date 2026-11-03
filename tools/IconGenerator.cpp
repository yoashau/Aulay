// Build application resources with the exact production tray renderer.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wil/common.h>
#include <wil/result.h>
#include "TrayIcon.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>

std::vector<BYTE> Png(IWICBitmap* image)
{
	auto imaging = winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
	winrt::com_ptr<IStream> stream;
	winrt::check_hresult(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()));
	winrt::com_ptr<IWICBitmapEncoder> encoder;
	winrt::check_hresult(imaging->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
	winrt::check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
	winrt::com_ptr<IWICBitmapFrameEncode> frame;
	winrt::check_hresult(encoder->CreateNewFrame(frame.put(), nullptr));
	winrt::check_hresult(frame->Initialize(nullptr));
	UINT width = 0, height = 0;
	winrt::check_hresult(image->GetSize(&width, &height));
	winrt::check_hresult(frame->SetSize(width, height));
	// PNG stores straight alpha; WIC converts the production premultiplied pixels.
	auto format = GUID_WICPixelFormat32bppBGRA;
	winrt::check_hresult(frame->SetPixelFormat(&format));
	winrt::com_ptr<IWICFormatConverter> straight;
	winrt::check_hresult(imaging->CreateFormatConverter(straight.put()));
	winrt::check_hresult(straight->Initialize(image, format, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
	winrt::check_hresult(frame->WriteSource(straight.get(), nullptr));
	winrt::check_hresult(frame->Commit());
	winrt::check_hresult(encoder->Commit());
	STATSTG stat {};
	winrt::check_hresult(stream->Stat(&stat, STATFLAG_NONAME));
	std::vector<BYTE> bytes(static_cast<size_t>(stat.cbSize.QuadPart));
	winrt::check_hresult(stream->Seek({}, STREAM_SEEK_SET, nullptr));
	ULONG read = 0;
	winrt::check_hresult(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read));
	winrt::check_bool(read == bytes.size());
	return bytes;
}

#pragma pack(push, 1)
struct IconEntry {
	BYTE width, height, colors, reserved;
	WORD planes, depth;
	DWORD length, offset;
};
#pragma pack(pop)

int wmain(int argc, wchar_t** argv)
{
	try {
		if (argc != 2)
			return 2;
		winrt::init_apartment();
		const int sizes[] = { 16, 20, 24, 28, 32, 40, 48, 64, 96, 128, 256 };
		std::vector<std::vector<BYTE>> frames;
		for (auto size : sizes) {
			auto image = AulayTrayIcon::Render(size, size, AulayTrayIcon::LightForeground);
			frames.push_back(Png(image.get()));
		}
		std::ofstream output(std::filesystem::path(argv[1]), std::ios::binary | std::ios::trunc);
		output.exceptions(std::ios::badbit | std::ios::failbit);
		const WORD header[] = { 0, 1, static_cast<WORD>(frames.size()) };
		output.write(reinterpret_cast<const char*>(header), sizeof(header));
		DWORD offset = sizeof(header) + static_cast<DWORD>(frames.size() * sizeof(IconEntry));
		for (size_t i = 0; i < frames.size(); ++i) {
			auto dimension = static_cast<BYTE>(sizes[i] == 256 ? 0 : sizes[i]);
			IconEntry entry { dimension, dimension, 0, 0, 1, 32, static_cast<DWORD>(frames[i].size()), offset };
			output.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
			offset += entry.length;
		}
		for (auto const& frame : frames)
			output.write(reinterpret_cast<const char*>(frame.data()), frame.size());
		output.close();
		std::cout << "Generated 11 application icon sizes with the production tray renderer.\n";
		return 0;
	} catch (winrt::hresult_error const& error) {
		std::wcerr << error.message().c_str() << '\n';
	} catch (std::exception const& error) {
		std::cerr << error.what() << '\n';
	}
	return 1;
}
