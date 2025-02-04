// SPDX-FileCopyrightText: 2002-2024 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "ChdFileReader.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/SmallString.h"
#include "common/StringUtil.h"

#include "libchdr/chd.h"
#include "fmt/format.h"
#include "xxhash.h"

static constexpr u32 MAX_PARENTS = 32; // Surely someone wouldn't be insane enough to go beyond this...
static std::vector<std::pair<std::string, chd_header>> s_chd_hash_cache; // <filename, header>
static std::recursive_mutex s_chd_hash_cache_mutex;

// Provides an implementation of core_file which allows us to control if the underlying FILE handle is freed.
// The lifetime of ChdCoreFileWrapper will be equal to that of the relevant chd_file,
// ChdCoreFileWrapper will also get destroyed if chd_open_core_file fails.
class ChdCoreFileWrapper
{
	DeclareNoncopyableObject(ChdCoreFileWrapper);

private:
	core_file m_core;
	std::FILE* m_file;
	bool m_free_file = false;

public:
	ChdCoreFileWrapper(std::FILE* file)
		: m_file{file}
	{
		m_core.argp = this;
		m_core.fsize = FSize;
		m_core.fread = FRead;
		m_core.fclose = FClose;
		m_core.fseek = FSeek;
	}

	~ChdCoreFileWrapper()
	{
		if (m_free_file)
			std::fclose(m_file);
	}

	core_file* GetCoreFile()
	{
		return &m_core;
	}

	static ChdCoreFileWrapper* FromCoreFile(core_file* file)
	{
		return reinterpret_cast<ChdCoreFileWrapper*>(file->argp);
	}

	void SetFileOwner(bool isOwner)
	{
		m_free_file = isOwner;
	}

private:
	static u64 FSize(core_file* file)
	{
		return static_cast<u64>(FileSystem::FSize64(FromCoreFile(file)->m_file));
	}

	static size_t FRead(void* buffer, size_t elmSize, size_t elmCount, core_file* file)
	{
		return std::fread(buffer, elmSize, elmCount, FromCoreFile(file)->m_file);
	}

	static int FClose(core_file* file)
	{
		// Destructor handles freeing the FILE handle.
		delete FromCoreFile(file);
		return 0;
	}

	static int FSeek(core_file* file, int64_t offset, int whence)
	{
		return FileSystem::FSeek64(FromCoreFile(file)->m_file, offset, whence);
	}
};

ChdFileReader::ChdFileReader() = default;

ChdFileReader::~ChdFileReader()
{
	pxAssert(!ChdFile);
}

static chd_file* OpenCHD(const std::string& filename, FileSystem::ManagedCFilePtr fp, Error* error, u32 recursion_level)
{
	chd_file* chd;
	ChdCoreFileWrapper* core_wrapper = new ChdCoreFileWrapper(fp.get());
	// libchdr will take ownership of core_wrapper, and will close/free it on failure.
	chd_error err = chd_open_core_file(core_wrapper->GetCoreFile(), CHD_OPEN_READ, nullptr, &chd);
	if (err == CHDERR_NONE)
	{
		// core_wrapper should manage fp.
		core_wrapper->SetFileOwner(true);
		fp.release();
		return chd;
	}
	else if (err != CHDERR_REQUIRES_PARENT)
	{
		Console.Error(fmt::format("Failed to open CHD '{}': {}", filename, chd_error_string(err)));
		Error::SetString(error, chd_error_string(err));
		return nullptr;
	}

	if (recursion_level >= MAX_PARENTS)
	{
		Console.Error(fmt::format("Failed to open CHD '{}': Too many parent files", filename));
		Error::SetString(error, "Too many parent files");
		return nullptr;
	}

	// Need to get the sha1 to look for.
	chd_header header;
	err = chd_read_header_file(fp.get(), &header);
	if (err != CHDERR_NONE)
	{
		Console.Error(fmt::format("Failed to read CHD header '{}': {}", filename, chd_error_string(err)));
		Error::SetString(error, chd_error_string(err));
		return nullptr;
	}

	// Find a chd with a matching sha1 in the same directory.
	// Have to do *.* and filter on the extension manually because Linux is case sensitive.
	chd_file* parent_chd = nullptr;
	const std::string parent_dir(Path::GetDirectory(filename));
	const std::unique_lock hash_cache_lock(s_chd_hash_cache_mutex);

	// Memoize which hashes came from what files, to avoid reading them repeatedly.
	for (auto it = s_chd_hash_cache.begin(); it != s_chd_hash_cache.end(); ++it)
	{
		if (!StringUtil::compareNoCase(parent_dir, Path::GetDirectory(it->first)))
			continue;

		if (!chd_is_matching_parent(&header, &it->second))
			continue;

		// Re-check the header, it might have changed since we last opened.
		chd_header parent_header;
		auto parent_fp = FileSystem::OpenManagedSharedCFile(it->first.c_str(), "rb", FileSystem::FileShareMode::DenyWrite);
		if (parent_fp && chd_read_header_file(parent_fp.get(), &parent_header) == CHDERR_NONE &&
			chd_is_matching_parent(&header, &parent_header))
		{
			// Need to take a copy of the string, because the parent might add to the list and invalidate the iterator.
			const std::string filename_to_open = it->first;

			// Match! Open this one.
			parent_chd = OpenCHD(filename_to_open, std::move(parent_fp), error, recursion_level + 1);
			if (parent_chd)
			{
				Console.WriteLn(
					fmt::format("Using parent CHD '{}' from cache for '{}'.", Path::GetFileName(filename_to_open), Path::GetFileName(filename)));
			}
		}

		// No point checking any others. Since we recursively call OpenCHD(), the iterator is invalidated anyway.
		break;
	}
	if (!parent_chd)
	{
		// Look for files in the same directory as the chd.
		FileSystem::FindResultsArray parent_files;
		FileSystem::FindFiles(
			parent_dir.c_str(), "*.*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES | FILESYSTEM_FIND_KEEP_ARRAY, &parent_files);
		for (FILESYSTEM_FIND_DATA& fd : parent_files)
		{
			if (!StringUtil::EndsWithNoCase(Path::GetExtension(fd.FileName), "chd"))
				continue;

			// Re-check the header, it might have changed since we last opened.
			chd_header parent_header;
			auto parent_fp = FileSystem::OpenManagedSharedCFile(fd.FileName.c_str(), "rb", FileSystem::FileShareMode::DenyWrite);
			if (!parent_fp || chd_read_header_file(parent_fp.get(), &parent_header) != CHDERR_NONE)
				continue;

			// Don't duplicate in the cache. But update it, in case the file changed.
			auto cache_it = std::find_if(s_chd_hash_cache.begin(), s_chd_hash_cache.end(), [&fd](const auto& it) { return it.first == fd.FileName; });
			if (cache_it != s_chd_hash_cache.end())
				std::memcpy(&cache_it->second, &parent_header, sizeof(parent_header));
			else
				s_chd_hash_cache.emplace_back(fd.FileName, parent_header);

			if (!chd_is_matching_parent(&header, &parent_header))
				continue;

			// Match! Open this one.
			parent_chd = OpenCHD(fd.FileName, std::move(parent_fp), error, recursion_level + 1);
			if (parent_chd)
			{
				Console.WriteLn(fmt::format("Using parent CHD '{}' for '{}'.", Path::GetFileName(fd.FileName), Path::GetFileName(filename)));
				break;
			}
		}
	}
	if (!parent_chd)
	{
		Console.Error(fmt::format("Failed to open CHD '{}': Failed to find parent CHD, it must be in the same directory.", filename));
		Error::SetString(error, "Failed to find parent CHD, it must be in the same directory.");
		return nullptr;
	}

	// Our last core file wrapper got freed, so make a new one.
	core_wrapper = new ChdCoreFileWrapper(fp.get());
	// Now try re-opening with the parent.
	err = chd_open_core_file(core_wrapper->GetCoreFile(), CHD_OPEN_READ, parent_chd, &chd);
	if (err != CHDERR_NONE)
	{
		Console.Error(fmt::format("Failed to open CHD '{}': {}", filename, chd_error_string(err)));
		Error::SetString(error, chd_error_string(err));
		return nullptr;
	}

	// core_wrapper should manage fp.
	core_wrapper->SetFileOwner(true);
	fp.release();
	return chd;
}

bool ChdFileReader::Open2(std::string filename, Error* error)
{
	Close2();

	m_filename = std::move(filename);

	auto fp = FileSystem::OpenManagedSharedCFile(m_filename.c_str(), "rb", FileSystem::FileShareMode::DenyWrite, error);
	if (!fp)
		return false;

	ChdFile = OpenCHD(m_filename, std::move(fp), error, 0);
	if (!ChdFile)
		return false;

	const chd_header* chd_header = chd_get_header(ChdFile);
	hunk_size = chd_header->hunkbytes;
	// CHD likes to use full 2448 byte blocks, but keeps the +24 offset of source ISOs
	// The rest of PCSX2 likes to use 2448 byte buffers, which can't fit that so trim blocks instead
	m_internalBlockSize = chd_header->unitbytes;

	// The file size in the header is incorrect, each track gets padded to a multiple of 4 frames.
	// (see chdman.cpp from MAME). Instead, we pull the real frame count from the TOC.
	u64 total_frames;
	if (ParseTOC(&total_frames))
	{
		file_size = total_frames * static_cast<u64>(chd_header->unitbytes);
	}
	else
	{
		Console.Warning("Failed to parse CHD TOC, file size may be incorrect.");
		file_size = static_cast<u64>(chd_header->unitbytes) * chd_header->unitcount;
	}

	return true;
}

bool ChdFileReader::Precache2(ProgressCallback* progress, Error* error)
{
	if (!CheckAvailableMemoryForPrecaching(chd_get_compressed_size(ChdFile), error))
		return false;

	progress->SetProgressRange(100);

	const auto callback = [](size_t pos, size_t total, void* param) -> bool {
		ProgressCallback* progress = static_cast<ProgressCallback*>(param);
		const u32 percent = static_cast<u32>((pos * 100) / total);
		progress->SetProgressValue(std::min<u32>(percent, 100));
		return !progress->IsCancelled();
	};

	const chd_error cerror = chd_precache_progress(ChdFile, callback, progress);
	if (cerror != CHDERR_NONE)
	{
		if (cerror != CHDERR_CANCELLED)
			Error::SetStringView(error, "Failed to read part of the file.");

		return false;
	}

	return true;
}

ThreadedFileReader::Chunk ChdFileReader::ChunkForOffset(u64 offset)
{
	Chunk chunk = {0};
	if (offset >= file_size)
	{
		chunk.chunkID = -1;
	}
	else
	{
		chunk.chunkID = offset / hunk_size;
		chunk.length = hunk_size;
		chunk.offset = chunk.chunkID * hunk_size;
	}
	return chunk;
}

int ChdFileReader::ReadChunk(void* dst, s64 chunkID)
{
	if (chunkID < 0)
		return -1;

	chd_error error = chd_read(ChdFile, chunkID, dst);
	if (error != CHDERR_NONE)
	{
		Console.Error("CDVD: chd_read returned error: %s", chd_error_string(error));
		return 0;
	}

	return hunk_size;
}

void ChdFileReader::Close2()
{
	if (ChdFile)
	{
		chd_close(ChdFile);
		ChdFile = nullptr;
	}
}

u32 ChdFileReader::GetBlockCount() const
{
	return (file_size - m_dataoffset) / m_internalBlockSize;
}

bool ChdFileReader::ParseTOC(u64* out_frame_count)
{
	u64 total_frames = 0;
	int max_found_track = -1;

	for (int search_index = 0;; search_index++)
	{
		char metadata_str[256];
		char type_str[256];
		char subtype_str[256];
		char pgtype_str[256];
		char pgsub_str[256];
		u32 metadata_length;

		int track_num = 0, frames = 0, pregap_frames = 0, postgap_frames = 0;
		chd_error err = chd_get_metadata(ChdFile, CDROM_TRACK_METADATA2_TAG, search_index, metadata_str, sizeof(metadata_str),
			&metadata_length, nullptr, nullptr);
		if (err == CHDERR_NONE)
		{
			if (std::sscanf(metadata_str, CDROM_TRACK_METADATA2_FORMAT, &track_num, type_str, subtype_str, &frames,
					&pregap_frames, pgtype_str, pgsub_str, &postgap_frames) != 8)
			{
				Console.Error(fmt::format("Invalid track v2 metadata: '{}'", metadata_str));
				return false;
			}
		}
		else
		{
			// try old version
			err = chd_get_metadata(ChdFile, CDROM_TRACK_METADATA_TAG, search_index, metadata_str, sizeof(metadata_str),
				&metadata_length, nullptr, nullptr);
			if (err != CHDERR_NONE)
			{
				// not found, so no more tracks
				break;
			}

			if (std::sscanf(metadata_str, CDROM_TRACK_METADATA_FORMAT, &track_num, type_str, subtype_str, &frames) != 4)
			{
				Console.Error(fmt::format("Invalid track metadata: '{}'", metadata_str));
				return false;
			}
		}

		DevCon.WriteLn(fmt::format("CHD Track {}: frames:{} pregap:{} postgap:{} type:{} sub:{} pgtype:{} pgsub:{}",
			track_num, frames, pregap_frames, postgap_frames, type_str, subtype_str, pgtype_str, pgsub_str));

		// PCSX2 doesn't currently support multiple tracks for CDs.
		if (track_num != 1)
		{
			Console.Warning(fmt::format("  Ignoring track {} in CHD.", track_num, frames));
			continue;
		}

		total_frames += static_cast<u64>(pregap_frames) + static_cast<u64>(frames) + static_cast<u64>(postgap_frames);
		max_found_track = std::max(max_found_track, track_num);
	}

	// No tracks in TOC?
	if (max_found_track < 0)
		return false;

	// Compute total data size.
	*out_frame_count = total_frames;
	return true;
}
