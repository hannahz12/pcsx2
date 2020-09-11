/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2019  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "PrecompiledHeader.h"

#include "App.h"
#include "Common.h"
#include "Counters.h"
#include "MainFrame.h"
#include "MemoryTypes.h"

#include "InputRecordingFile.h"

#ifndef DISABLE_RECORDING

void InputRecordingFileHeader::Init()
{
	memset(author, 0, ArraySize(author));
	memset(gameName, 0, ArraySize(gameName));
}

void InputRecordingFileHeader::SetEmulatorVersion()
{
	wxString emuVersion = wxString::Format("%s-%d.%d.%d", pxGetAppName().c_str(), PCSX2_VersionHi, PCSX2_VersionMid, PCSX2_VersionLo);
	int max = ArraySize(emu) - 1;
	strncpy(emu, emuVersion.c_str(), max);
	emu[max] = 0;
}

void InputRecordingFileHeader::SetAuthor(wxString _author)
{
	int max = ArraySize(author) - 1;
	strncpy(author, _author.c_str(), max);
	author[max] = 0;
}

void InputRecordingFileHeader::SetGameName(wxString _gameName)
{
	int max = ArraySize(gameName) - 1;
	strncpy(gameName, _gameName.c_str(), max);
	gameName[max] = 0;
}

bool InputRecordingFile::Close()
{
	if (recordingFile == NULL)
	{
		return false;
	}
	fclose(recordingFile);
	recordingFile = NULL;
	filename = "";
	return true;
}

const wxString &InputRecordingFile::GetFilename()
{
	return filename;
}

InputRecordingFileHeader &InputRecordingFile::GetHeader()
{
	return header;
}

unsigned long &InputRecordingFile::GetTotalFrames()
{
	return totalFrames;
}

unsigned long &InputRecordingFile::GetUndoCount()
{
	return undoCount;
}

bool InputRecordingFile::FromSaveState()
{
	return savestate.fromSavestate;
}

void InputRecordingFile::IncrementUndoCount()
{
	undoCount++;
	if (recordingFile == NULL)
	{
		return;
	}
	fseek(recordingFile, seekpointUndoCount, SEEK_SET);
	fwrite(&undoCount, 4, 1, recordingFile);
}

bool InputRecordingFile::open(const wxString path, bool newRecording)
{
	if (newRecording)
	{
		if ((recordingFile = wxFopen(path, L"wb+")) != nullptr)
		{
			filename = path;
			totalFrames = 0;
			undoCount = 0;
			header.Init();
			return true;
		}
	}
	else if ((recordingFile = wxFopen(path, L"rb+")) != nullptr)
	{
		if (verifyRecordingFileHeader())
		{
			filename = path;
			return true;
		}
		Close();
		recordingConLog(wxString::Format("[REC]: Input recording file header is invalid\n"));
		return false;
	}
	recordingConLog(wxString::Format("[REC]: Input recording file opening failed. Error - %s\n", strerror(errno)));
	return false;
}

bool InputRecordingFile::OpenNew(const wxString path, bool fromSavestate)
{
	if (fromSavestate)
	{
		if (CoreThread.IsOpen())
		{
			if (open(path, true))
			{
				savestate.fromSavestate = true;
				if (wxFileExists(path + "_SaveState.p2s"))
				{
					wxCopyFile(path + "_SaveState.p2s", path + "_SaveState.p2s.bak", true);
				}
				StateCopy_SaveToFile(path + "_SaveState.p2s");
				return true;
			}
		}
		else
			recordingConLog(L"[REC]: Game is not open, aborting playing input recording which starts on a save-state.\n");
		return false;
	}
	else if (open(path, true))
	{
		savestate.fromSavestate = false;
		sApp.SysExecute();
		return true;
	}
	return open(path, true);
}

bool InputRecordingFile::OpenExisting(const wxString path)
{
	return open(path, false);
}

bool InputRecordingFile::ReadKeyBuffer(u8 &result, const uint &frame, const uint port, const uint bufIndex)
{
	if (recordingFile == NULL)
	{
		return false;
	}

	long seek = getRecordingBlockSeekPoint(frame) + controllerInputBytes * port + bufIndex;
	if (fseek(recordingFile, seek, SEEK_SET) != 0)
	{
		return false;
	}
	if (fread(&result, 1, 1, recordingFile) != 1)
	{
		return false;
	}

	return true;
}

void InputRecordingFile::SetTotalFrames(unsigned long frame)
{
	if (recordingFile == NULL || totalFrames >= frame)
	{
		return;
	}
	totalFrames = frame;
	fseek(recordingFile, seekpointTotalFrames, SEEK_SET);
	fwrite(&totalFrames, 4, 1, recordingFile);
}

bool InputRecordingFile::WriteHeader()
{
	if (recordingFile == NULL)
	{
		return false;
	}
	rewind(recordingFile);
	if (fwrite(&header, sizeof(InputRecordingFileHeader), 1, recordingFile) != 1
		|| fwrite(&totalFrames, 4, 1, recordingFile) != 1
		|| fwrite(&undoCount, 4, 1, recordingFile) != 1
		|| fwrite(&savestate, 1, 1, recordingFile) != 1)
	{
		return false;
	}
	return true;
}

bool InputRecordingFile::WriteKeyBuffer(const uint &frame, const uint port, const uint bufIndex, const u8 &buf)
{
	if (recordingFile == NULL)
	{
		return false;
	}

	long seek = getRecordingBlockSeekPoint(frame) + 18 * port + bufIndex;

	if (fseek(recordingFile, seek, SEEK_SET) != 0
		|| fwrite(&buf, 1, 1, recordingFile) != 1)
	{
		return false;
	}

	fflush(recordingFile);
	return true;
}

long InputRecordingFile::getRecordingBlockSeekPoint(const long &frame)
{
	return headerSize + sizeof(bool) + frame * inputBytesPerFrame;
}

bool InputRecordingFile::verifyRecordingFileHeader()
{
	if (recordingFile == NULL)
	{
		return false;
	}
	// Verify header contents
	rewind(recordingFile);
	if (fread(&header, sizeof(InputRecordingFileHeader), 1, recordingFile) != 1
		|| fread(&totalFrames, 4, 1, recordingFile) != 1
		|| fread(&undoCount, 4, 1, recordingFile) != 1
		|| fread(&savestate.fromSavestate, sizeof(bool), 1, recordingFile) != 1)
	{
		return false;
	}
	
	// Check for current verison
	if (header.version != 1)
	{
		recordingConLog(wxString::Format("[REC]: Input recording file is not a supported version - %d\n", header.version));
		return false;
	}
	return true;
}

bool InputRecordingFile::writeSaveState() {
	if (recordingFile == NULL)
	{
		return false;
	}

	fseek(recordingFile, seekpointSaveStateHeader, SEEK_SET);
	if (fwrite(&savestate.fromSavestate, sizeof(bool), 1, recordingFile) != 1)
	{
		return false;
	}

	return true;
}
#endif
