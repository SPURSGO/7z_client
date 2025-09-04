// File Compression Demo
// This demo demonstrates how to compress a list of files into a 7z archive

#include "StdAfx.h"

#include <stdio.h>
#include <iostream>
#include <vector>
#include <string>

#include "cpp/Common/MyWindows.h"
#include "cpp/Common/MyInitGuid.h"
#include "cpp/Common/Defs.h"
#include "cpp/Common/IntToString.h"
#include "cpp/Common/StringConvert.h"
#include "cpp/Windows/DLL.h"
#include "cpp/Windows/FileDir.h"
#include "cpp/Windows/FileFind.h"
#include "cpp/Windows/FileName.h"
#include "cpp/Windows/NtCheck.h"
#include "cpp/Windows/PropVariant.h"
#include "cpp/Windows/PropVariantConv.h"
#include "cpp/7zip/Common/FileStreams.h"
#include "cpp/7zip/Archive/IArchive.h"
#include "cpp/7zip/IPassword.h"
#include "C/7zVersion.h"

#ifdef _WIN32
extern HINSTANCE g_hInstance;
HINSTANCE g_hInstance = NULL;
#endif

Z7_DIAGNOSTIC_IGNORE_CAST_FUNCTION

// Define 7z format GUID
#define DEFINE_GUID_ARC(name, id) Z7_DEFINE_GUID(name, \
  0x23170F69, 0x40C1, 0x278A, 0x10, 0x00, 0x00, 0x01, 0x10, id, 0x00, 0x00);

enum {
  kId_7z = 7,
};

DEFINE_GUID_ARC(CLSID_Format, kId_7z)

using namespace NWindows;
using namespace NFile;
using namespace NDir;

#ifdef _WIN32
#define kDllName "7z.dll"
#else
#define kDllName "7z.so"
#endif

// Utility functions for output
static void Convert_UString_to_AString(const UString &s, AString &temp) {
  int codePage = CP_OEMCP;
  UnicodeStringToMultiByte2(temp, s, (UINT)codePage);
}

static void Print(const char *s) {
  fputs(s, stdout);
}

static void Print(const AString &s) {
  Print(s.Ptr());
}

static void Print(const UString &s) {
  AString as;
  Convert_UString_to_AString(s, as);
  Print(as);
}

static void PrintNewLine() {
  Print("\n");
}

static void PrintError(const char *message) {
  Print("Error: ");
  Print(message);
  PrintNewLine();
}

static void PrintError(const char *message, const FString &name) {
  PrintError(message);
  Print(name);
  PrintNewLine();
}

static void PrintSuccess(const char *message) {
  Print("Success: ");
  Print(message);
  PrintNewLine();
}

// Directory item structure for files to be compressed
struct CDirItem: public NWindows::NFile::NFind::CFileInfoBase {
  UString Path_For_Handler;
  FString FullPath; // for filesystem

  CDirItem(const NWindows::NFile::NFind::CFileInfo &fi):
      CFileInfoBase(fi) {}
};

// Archive update callback class
class CArchiveUpdateCallback Z7_final:
  public IArchiveUpdateCallback2,
  public ICryptoGetTextPassword2,
  public CMyUnknownImp {
  Z7_IFACES_IMP_UNK_2(IArchiveUpdateCallback2, ICryptoGetTextPassword2)
  Z7_IFACE_COM7_IMP(IProgress)
  Z7_IFACE_COM7_IMP(IArchiveUpdateCallback)

public:
  CRecordVector<UInt64> VolumesSizes;
  UString VolName;
  UString VolExt;

  FString DirPrefix;
  const CObjectVector<CDirItem> *DirItems;

  bool PasswordIsDefined;
  UString Password;
  bool AskPassword;

  bool m_NeedBeClosed;

  FStringVector FailedFiles;
  CRecordVector<HRESULT> FailedCodes;

  CArchiveUpdateCallback():
      DirItems(NULL),
      PasswordIsDefined(false),
      AskPassword(false) {
    m_NeedBeClosed = false;
  }

  HRESULT Finilize();

  void Init(const CObjectVector<CDirItem> *dirItems) {
    DirItems = dirItems;
    m_NeedBeClosed = false;
    FailedFiles.Clear();
    FailedCodes.Clear();
  }
};

Z7_COM7F_IMF(CArchiveUpdateCallback::SetTotal(UInt64  size )) {
  return S_OK;
}

Z7_COM7F_IMF(CArchiveUpdateCallback::SetCompleted(const UInt64 *  completeValue )) {
  return S_OK;
}

Z7_COM7F_IMF(CArchiveUpdateCallback::GetUpdateItemInfo(UInt32  index ,
      Int32 *newData, Int32 *newProperties, UInt32 *indexInArchive)) {
  if (newData)
    *newData = BoolToInt(true);
  if (newProperties)
    *newProperties = BoolToInt(true);
  if (indexInArchive)
    *indexInArchive = (UInt32)(Int32)-1;
  return S_OK;
}

Z7_COM7F_IMF(CArchiveUpdateCallback::GetProperty(UInt32 index, PROPID propID, PROPVARIANT *value)) {
  NCOM::CPropVariant prop;
  
  if (propID == kpidIsAnti) {
    prop = false;
    prop.Detach(value);
    return S_OK;
  }

  const CDirItem &dirItem = (*DirItems)[index];
  switch (propID) {
    case kpidPath:  prop = dirItem.Path_For_Handler; break;
    case kpidIsDir:  prop = dirItem.IsDir(); break;
    case kpidSize:  prop = dirItem.Size; break;
    case kpidCTime:  PropVariant_SetFrom_FiTime(prop, dirItem.CTime); break;
    case kpidATime:  PropVariant_SetFrom_FiTime(prop, dirItem.ATime); break;
    case kpidMTime:  PropVariant_SetFrom_FiTime(prop, dirItem.MTime); break;
    case kpidAttrib:  prop = (UInt32)dirItem.GetWinAttrib(); break;  // 修复：使用GetWinAttrib()方法
    case kpidPosixAttrib: prop = (UInt32)dirItem.GetPosixAttrib(); break;
  }
  prop.Detach(value);
  return S_OK;
}

HRESULT CArchiveUpdateCallback::Finilize() {
  if (m_NeedBeClosed) {
    PrintNewLine();
    m_NeedBeClosed = false;
  }
  return S_OK;
}

Z7_COM7F_IMF(CArchiveUpdateCallback::GetStream(UInt32 index, ISequentialInStream **inStream)) {
  RINOK(Finilize())

  const CDirItem &dirItem = (*DirItems)[index];
  if (dirItem.IsDir())
    return S_OK;

  {
    CInFileStream *inStreamSpec = new CInFileStream;
    CMyComPtr<ISequentialInStream> inStreamLoc(inStreamSpec);
    if (!inStreamSpec->Open(dirItem.FullPath)) {
      DWORD sysError = ::GetLastError();
      FailedCodes.Add(HRESULT_FROM_WIN32(sysError));
      FailedFiles.Add(dirItem.FullPath);
      return S_FALSE;
    }
    *inStream = inStreamLoc.Detach();
  }
  return S_OK;
}

Z7_COM7F_IMF(CArchiveUpdateCallback::SetOperationResult(Int32 /* operationResult */)) {
  m_NeedBeClosed = true;
  return S_OK;
}

Z7_COM7F_IMF(CArchiveUpdateCallback::GetVolumeSize(UInt32 index, UInt64 *size)) {
  if (VolumesSizes.Size() == 0)
    return S_FALSE;
  if (index >= (UInt32)VolumesSizes.Size())
    index = VolumesSizes.Size() - 1;
  *size = VolumesSizes[index];
  return S_OK;
}

Z7_COM7F_IMF(CArchiveUpdateCallback::GetVolumeStream(UInt32 index, ISequentialOutStream **volumeStream)) {
  wchar_t temp[16];
  ConvertUInt32ToString(index + 1, temp);
  UString res = temp;
  while (res.Len() < 2)
    res.InsertAtFront(L'0');
  UString fileName = VolName;
  fileName.Add_Dot();
  fileName += res;
  fileName += VolExt;
  COutFileStream *streamSpec = new COutFileStream;
  CMyComPtr<ISequentialOutStream> streamLoc(streamSpec);
  if (!streamSpec->Create_NEW(us2fs(fileName)))  // 修复：使用Create_NEW方法
    return GetLastError_noZero_HRESULT();        // 修复：使用正确的错误返回方式
  *volumeStream = streamLoc.Detach();
  return S_OK;
}

Z7_COM7F_IMF(CArchiveUpdateCallback::CryptoGetTextPassword2(Int32 *passwordIsDefined, BSTR *password)) {
  if (!PasswordIsDefined) {
    if (AskPassword) {
      // Here you can implement password input logic
      // For demo purposes, we'll use no password
      *passwordIsDefined = BoolToInt(false);
      return S_OK;
    }
  }
  *passwordIsDefined = BoolToInt(PasswordIsDefined);
  return StringToBstr(Password, password);
}

// Main compression function
class FileCompressor {
public:
  // Compress specified files into a 7z archive
  static bool CompressFiles(const std::vector<std::string>& file_paths, 
                           const std::string& archive_path) {
    Print("Starting file compression...\n");
    
    // Load 7z library
    FString dll_prefix;
    dll_prefix = NDLL::GetModuleDirPrefix();

    NDLL::CLibrary lib;
    if (!lib.Load(dll_prefix + FTEXT(kDllName))) {
      PrintError("Cannot load 7-zip library");
      return false;
    }

    // Get CreateObject function
    Func_CreateObject f_CreateObject = Z7_GET_PROC_ADDRESS(
        Func_CreateObject, lib.Get_HMODULE(), "CreateObject");
    if (!f_CreateObject) {
      PrintError("Cannot get CreateObject function");
      return false;
    }

    // Collect all files (including files from directories)
    CObjectVector<CDirItem> dir_items;
    if (!CollectFilesFromPaths(file_paths, dir_items)) {
      return false;
    }

    if (dir_items.Size() == 0) {
      PrintError("No files found to compress");
      return false;
    }

    Print("Total files to compress: ");
    char count_str[32];
    sprintf_s(count_str, "%u", (unsigned)dir_items.Size());
    Print(count_str);
    PrintNewLine();

    // Create output archive file
    FString archive_name = CmdStringToFString(archive_path.c_str());
    COutFileStream* out_file_stream_spec = new COutFileStream;
    CMyComPtr<IOutStream> out_file_stream = out_file_stream_spec;
    if (!out_file_stream_spec->Create_NEW(archive_name)) {
      PrintError("Cannot create archive file", archive_name);
      return false;
    }

    // Create archive object
    CMyComPtr<IOutArchive> out_archive;
    if (f_CreateObject(&CLSID_Format, &IID_IOutArchive, 
                      (void**)&out_archive) != S_OK) {
      PrintError("Cannot create archive object");
      return false;
    }

    // Create update callback
    CArchiveUpdateCallback* update_callback_spec = new CArchiveUpdateCallback;
    CMyComPtr<IArchiveUpdateCallback2> update_callback(update_callback_spec);
    update_callback_spec->Init(&dir_items);
    update_callback_spec->PasswordIsDefined = false;

    // Perform compression
    Print("Compressing files...\n");
    HRESULT result = out_archive->UpdateItems(out_file_stream, 
                                             dir_items.Size(), 
                                             update_callback);
    
    update_callback_spec->Finilize();
    
    if (result != S_OK) {
      PrintError("Compression failed");
      return false;
    }
    
    // Check for failed files
    if (update_callback_spec->FailedFiles.Size() != 0) {
      PrintError("Some files failed to compress:");
      for (unsigned i = 0; i < update_callback_spec->FailedFiles.Size(); i++) {
        Print("  ");
        Print(update_callback_spec->FailedFiles[i]);
        PrintNewLine();
      }
      return false;
    }

    PrintSuccess("Files compressed successfully!");
    Print("Archive created: ");
    Print(archive_path.c_str());
    PrintNewLine();
    
    return true;
  }

private:
  // Recursively collect files from given paths (files and directories)
  static bool CollectFilesFromPaths(const std::vector<std::string>& file_paths,
                                   CObjectVector<CDirItem>& dir_items) {
    for (const auto& file_path : file_paths) {
      FString fs_path = CmdStringToFString(file_path.c_str());
      
      NFind::CFileInfo file_info;
      if (!file_info.Find(fs_path)) {
        PrintError("Cannot find file or directory", fs_path);
        return false;
      }

      if (file_info.IsDir()) {
        // Process directory recursively
        Print("Processing directory: ");
        Print(file_path.c_str());
        PrintNewLine();
        
        if (!CollectFilesFromDirectory(fs_path, L"", dir_items)) {
          return false;
        }
      } else {
        // Process single file
        CDirItem dir_item(file_info);
        dir_item.Path_For_Handler = fs2us(fs_path.Ptr(fs_path.ReverseFind_PathSepar() + 1));
        dir_item.FullPath = fs_path;
        dir_items.Add(dir_item);
        
        Print("Added file: ");
        Print(file_path.c_str());
        PrintNewLine();
      }
    }
    return true;
  }

  // Recursively collect files from a directory
  static bool CollectFilesFromDirectory(const FString& dir_path,
                                       const UString& relative_path,
                                       CObjectVector<CDirItem>& dir_items) {
    NFind::CEnumerator enumerator;
    enumerator.SetDirPrefix(dir_path);
    
    NFind::CFileInfo file_info;
    while (enumerator.Next(file_info)) {
      if (file_info.IsDir()) {
        // Skip current and parent directory entries
        if (file_info.Name.IsEqualTo(".") || file_info.Name.IsEqualTo("..")) {
          continue;
        }
        
        // Recursively process subdirectory
        FString sub_dir_path = dir_path;
        sub_dir_path.Add_PathSepar();
        sub_dir_path += file_info.Name;
        
        UString sub_relative_path = relative_path;
        if (!sub_relative_path.IsEmpty()) {
          sub_relative_path.Add_PathSepar();
        }
        sub_relative_path += fs2us(file_info.Name);
        
        if (!CollectFilesFromDirectory(sub_dir_path, sub_relative_path, dir_items)) {
          return false;
        }
      } else {
        // Add file to collection
        CDirItem dir_item(file_info);
        
        // Set relative path for archive
        UString file_relative_path = relative_path;
        if (!file_relative_path.IsEmpty()) {
          file_relative_path.Add_PathSepar();
        }
        file_relative_path += fs2us(file_info.Name);
        dir_item.Path_For_Handler = file_relative_path;
        
        // Set full path for file system
        dir_item.FullPath = dir_path;
        dir_item.FullPath.Add_PathSepar();
        dir_item.FullPath += file_info.Name;
        
        dir_items.Add(dir_item);
        
        Print("  Found file: ");
        Print(fs2us(file_relative_path));
        PrintNewLine();
      }
    }
    return true;
  }

  static FString CmdStringToFString(const char* s) {
    return us2fs(GetUnicodeString(s));
  }
};

// Demo main function
int main() {
  Print("=== File Compression Demo ===\n");
  Print("This demo compresses specified files into a 7z archive.\n\n");

  // Example file list to compress
  // You can modify this list to include your own files
  std::vector<std::string> files_to_compress = {
    // Add your file paths here
    // Example: "C:\\temp\\file1.txt",
    // Example: "C:\\temp\\file2.txt",
    // For demo purposes, we'll try to compress some common files
    //"readme.txt",  // This file should exist in the project root
    //R"(C:\Users\ewing\Desktop\7z_test\)",
    R"(C:\Users\ewing\Desktop\7z_test\7zFM.exe)",
    R"(C:\Users\ewing\Desktop\7z_test\7zG.exe)",
    R"(C:\Users\ewing\Desktop\7z_test\7-zip.chm)",
    R"(C:\Users\ewing\Desktop\7z_test\7-zip.dll)",
    R"(C:\Users\ewing\Desktop\7z_test\7-zip32.dll)",
    R"(C:\Users\ewing\Desktop\7z_test\Uninstall.exe)",
  };

  // Output archive path
  //std::string output_archive = "demo_archive.7z";
  std::string output_archive = R"(C:\Users\ewing\Desktop\archive_temp\demo_archive.7z)";

  Print("Files to compress:\n");
  for (const auto& file : files_to_compress) {
    Print("  - ");
    Print(file.c_str());
    PrintNewLine();
  }
  PrintNewLine();

  Print("Output archive: ");
  Print(output_archive.c_str());
  PrintNewLine();
  PrintNewLine();

  // Perform compression
  bool success = FileCompressor::CompressFiles(files_to_compress, output_archive);
  
  if (success) {
    Print("\n=== Compression completed successfully! ===\n");
    return 0;
  } else {
    Print("\n=== Compression failed! ===\n");
    return 1;
  }
}