#pragma once
#include "FnvHash.hpp"

std::unordered_map<uint32_t, const wchar_t*> hashToStrMap;

#pragma pack(push, 1)
struct YMOData
{
	uint16_t len;
	struct
	{
		uint32_t hash;
		uint16_t offset;
	} table[1];
};
#pragma pack(pop)

void LoadTranslateData()
{
	hashToStrMap.clear();

	auto hRes = FindResourceExW(g_hInst, L"YMO", MAKEINTRESOURCEW(1), GetThreadUILanguage());
	if (hRes)
	{
		auto resourceSize = static_cast<size_t>(SizeofResource(g_hInst, hRes));
		constexpr size_t headerSize = offsetof(YMOData, table);
		if (resourceSize < headerSize)
			return;

		auto hResData = LoadResource(g_hInst, hRes);
		if (hResData)
		{
			auto resourceBytes = reinterpret_cast<const uint8_t*>(LockResource(hResData));
			auto ymo = reinterpret_cast<const YMOData*>(resourceBytes);
			if (ymo)
			{
				auto tableSize = static_cast<size_t>(ymo->len) * sizeof(ymo->table[0]);
				if (tableSize > resourceSize - headerSize)
					return;
				auto stringsOffset = headerSize + tableSize;

				hashToStrMap.reserve(ymo->len);

				for (uint16_t i = 0; i < ymo->len; ++i)
				{
					auto hash = ymo->table[i].hash;
					auto offset = static_cast<size_t>(ymo->table[i].offset);
					if (offset < stringsOffset || offset % sizeof(wchar_t) != 0 ||
						offset >= resourceSize || (resourceSize - offset) < sizeof(wchar_t))
						continue;

					auto str = reinterpret_cast<const wchar_t*>(resourceBytes + offset);
					auto remainingCharacters = (resourceSize - offset) / sizeof(wchar_t);
					if (wmemchr(str, L'\0', remainingCharacters) == nullptr)
						continue;

					hashToStrMap.emplace(hash, str);
				}
			}
		}
	}
}

const wchar_t* Translate(const wchar_t* str)
{
	static std::unordered_map<const wchar_t*, const wchar_t*> ptrToStrMap;

	auto translation = str;

	auto i = ptrToStrMap.find(str);
	if (i == ptrToStrMap.end())
	{
		auto hash = fnv1a_32(str, wcslen(str) * sizeof(wchar_t));
		auto j = hashToStrMap.find(hash);
		if (j != hashToStrMap.end())
			translation = j->second;

		ptrToStrMap.emplace(str, translation);
	}
	else
		translation = i->second;

	return translation;
}

const wchar_t* TranslateContext(const wchar_t* str, const wchar_t* ctxtStr)
{
	auto translation = Translate(ctxtStr);
	if (translation == ctxtStr)
		return str;
	return translation;
}

#define _(str) Translate(str)
#define C_(ctxt, str) TranslateContext(str, ctxt L"\004" str)
