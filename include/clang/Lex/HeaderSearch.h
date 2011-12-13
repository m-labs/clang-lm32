//===--- HeaderSearch.h - Resolve Header File Locations ---------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the HeaderSearch interface.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LEX_HEADERSEARCH_H
#define LLVM_CLANG_LEX_HEADERSEARCH_H

#include "clang/Lex/DirectoryLookup.h"
#include "clang/Lex/ModuleMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Allocator.h"
#include <vector>

namespace clang {
  
class DiagnosticsEngine;  
class ExternalIdentifierLookup;
class FileEntry;
class FileManager;
class IdentifierInfo;

/// HeaderFileInfo - The preprocessor keeps track of this information for each
/// file that is #included.
struct HeaderFileInfo {
  /// isImport - True if this is a #import'd or #pragma once file.
  unsigned isImport : 1;

  /// isPragmaOnce - True if this is  #pragma once file.
  unsigned isPragmaOnce : 1;

  /// DirInfo - Keep track of whether this is a system header, and if so,
  /// whether it is C++ clean or not.  This can be set by the include paths or
  /// by #pragma gcc system_header.  This is an instance of
  /// SrcMgr::CharacteristicKind.
  unsigned DirInfo : 2;

  /// \brief Whether this header file info was supplied by an external source.
  unsigned External : 1;
  
  /// \brief Whether this structure is considered to already have been
  /// "resolved", meaning that it was loaded from the external source.
  unsigned Resolved : 1;
  
  /// \brief Whether this is a header inside a framework that is currently
  /// being built. 
  ///
  /// When a framework is being built, the headers have not yet been placed
  /// into the appropriate framework subdirectories, and therefore are
  /// provided via a header map. This bit indicates when this is one of
  /// those framework headers.
  unsigned IndexHeaderMapHeader : 1;
  
  /// NumIncludes - This is the number of times the file has been included
  /// already.
  unsigned short NumIncludes;

  /// \brief The ID number of the controlling macro.
  ///
  /// This ID number will be non-zero when there is a controlling
  /// macro whose IdentifierInfo may not yet have been loaded from
  /// external storage.
  unsigned ControllingMacroID;

  /// ControllingMacro - If this file has a #ifndef XXX (or equivalent) guard
  /// that protects the entire contents of the file, this is the identifier
  /// for the macro that controls whether or not it has any effect.
  ///
  /// Note: Most clients should use getControllingMacro() to access
  /// the controlling macro of this header, since
  /// getControllingMacro() is able to load a controlling macro from
  /// external storage.
  const IdentifierInfo *ControllingMacro;

  /// \brief If this header came from a framework include, this is the name
  /// of the framework.
  StringRef Framework;
  
  HeaderFileInfo()
    : isImport(false), isPragmaOnce(false), DirInfo(SrcMgr::C_User), 
      External(false), Resolved(false), IndexHeaderMapHeader(false),
      NumIncludes(0), ControllingMacroID(0), ControllingMacro(0)  {}

  /// \brief Retrieve the controlling macro for this header file, if
  /// any.
  const IdentifierInfo *getControllingMacro(ExternalIdentifierLookup *External);
  
  /// \brief Determine whether this is a non-default header file info, e.g.,
  /// it corresponds to an actual header we've included or tried to include.
  bool isNonDefault() const {
    return isImport || isPragmaOnce || NumIncludes || ControllingMacro || 
      ControllingMacroID;
  }
};

/// \brief An external source of header file information, which may supply
/// information about header files already included.
class ExternalHeaderFileInfoSource {
public:
  virtual ~ExternalHeaderFileInfoSource();
  
  /// \brief Retrieve the header file information for the given file entry.
  ///
  /// \returns Header file information for the given file entry, with the
  /// \c External bit set. If the file entry is not known, return a 
  /// default-constructed \c HeaderFileInfo.
  virtual HeaderFileInfo GetHeaderFileInfo(const FileEntry *FE) = 0;
};
  
/// HeaderSearch - This class encapsulates the information needed to find the
/// file referenced by a #include or #include_next, (sub-)framework lookup, etc.
class HeaderSearch {
  FileManager &FileMgr;
  DiagnosticsEngine &Diags;
  /// #include search path information.  Requests for #include "x" search the
  /// directory of the #including file first, then each directory in SearchDirs
  /// consecutively. Requests for <x> search the current dir first, then each
  /// directory in SearchDirs, starting at AngledDirIdx, consecutively.  If
  /// NoCurDirSearch is true, then the check for the file in the current
  /// directory is suppressed.
  std::vector<DirectoryLookup> SearchDirs;
  unsigned AngledDirIdx;
  unsigned SystemDirIdx;
  bool NoCurDirSearch;

  /// \brief The path to the module cache.
  std::string ModuleCachePath;
  
  /// FileInfo - This contains all of the preprocessor-specific data about files
  /// that are included.  The vector is indexed by the FileEntry's UID.
  ///
  std::vector<HeaderFileInfo> FileInfo;

  /// LookupFileCache - This is keeps track of each lookup performed by
  /// LookupFile.  The first part of the value is the starting index in
  /// SearchDirs that the cached search was performed from.  If there is a hit
  /// and this value doesn't match the current query, the cache has to be
  /// ignored.  The second value is the entry in SearchDirs that satisfied the
  /// query.
  llvm::StringMap<std::pair<unsigned, unsigned>, llvm::BumpPtrAllocator>
    LookupFileCache;

  /// FrameworkMap - This is a collection mapping a framework or subframework
  /// name like "Carbon" to the Carbon.framework directory.
  llvm::StringMap<const DirectoryEntry *, llvm::BumpPtrAllocator>
    FrameworkMap;

  /// HeaderMaps - This is a mapping from FileEntry -> HeaderMap, uniquing
  /// headermaps.  This vector owns the headermap.
  std::vector<std::pair<const FileEntry*, const HeaderMap*> > HeaderMaps;

  /// \brief The mapping between modules and headers.
  ModuleMap ModMap;
  
  /// \brief Describes whether a given directory has a module map in it.
  llvm::DenseMap<const DirectoryEntry *, bool> DirectoryHasModuleMap;
  
  /// \brief Uniqued set of framework names, which is used to track which 
  /// headers were included as framework headers.
  llvm::StringSet<llvm::BumpPtrAllocator> FrameworkNames;
  
  /// \brief Entity used to resolve the identifier IDs of controlling
  /// macros into IdentifierInfo pointers, as needed.
  ExternalIdentifierLookup *ExternalLookup;

  /// \brief Entity used to look up stored header file information.
  ExternalHeaderFileInfoSource *ExternalSource;
  
  // Various statistics we track for performance analysis.
  unsigned NumIncluded;
  unsigned NumMultiIncludeFileOptzn;
  unsigned NumFrameworkLookups, NumSubFrameworkLookups;

  // HeaderSearch doesn't support default or copy construction.
  explicit HeaderSearch();
  explicit HeaderSearch(const HeaderSearch&);
  void operator=(const HeaderSearch&);
public:
  HeaderSearch(FileManager &FM, DiagnosticsEngine &Diags);
  ~HeaderSearch();

  FileManager &getFileMgr() const { return FileMgr; }

  /// SetSearchPaths - Interface for setting the file search paths.
  ///
  void SetSearchPaths(const std::vector<DirectoryLookup> &dirs,
                      unsigned angledDirIdx, unsigned systemDirIdx,
                      bool noCurDirSearch) {
    assert(angledDirIdx <= systemDirIdx && systemDirIdx <= dirs.size() &&
        "Directory indicies are unordered");
    SearchDirs = dirs;
    AngledDirIdx = angledDirIdx;
    SystemDirIdx = systemDirIdx;
    NoCurDirSearch = noCurDirSearch;
    //LookupFileCache.clear();
  }

  /// AddSearchPath - Add an additional search path.
  void AddSearchPath(const DirectoryLookup &dir, bool isAngled) {
    unsigned idx = isAngled ? SystemDirIdx : AngledDirIdx;
    SearchDirs.insert(SearchDirs.begin() + idx, dir);
    if (!isAngled)
      AngledDirIdx++;
    SystemDirIdx++;
  }

  /// \brief Set the path to the module cache.
  void setModuleCachePath(StringRef CachePath) {
    ModuleCachePath = CachePath;
  }
  
  /// \brief Retrieve the path to the module cache.
  StringRef getModuleCachePath() const { return ModuleCachePath; }
  
  /// ClearFileInfo - Forget everything we know about headers so far.
  void ClearFileInfo() {
    FileInfo.clear();
  }

  void SetExternalLookup(ExternalIdentifierLookup *EIL) {
    ExternalLookup = EIL;
  }

  ExternalIdentifierLookup *getExternalLookup() const {
    return ExternalLookup;
  }
  
  /// \brief Set the external source of header information.
  void SetExternalSource(ExternalHeaderFileInfoSource *ES) {
    ExternalSource = ES;
  }
  
  /// LookupFile - Given a "foo" or <foo> reference, look up the indicated file,
  /// return null on failure.
  ///
  /// \returns If successful, this returns 'UsedDir', the DirectoryLookup member
  /// the file was found in, or null if not applicable.
  ///
  /// \param isAngled indicates whether the file reference is a <> reference.
  ///
  /// \param CurDir If non-null, the file was found in the specified directory
  /// search location.  This is used to implement #include_next.
  ///
  /// \param CurFileEnt If non-null, indicates where the #including file is, in
  /// case a relative search is needed.
  ///
  /// \param SearchPath If non-null, will be set to the search path relative
  /// to which the file was found. If the include path is absolute, SearchPath
  /// will be set to an empty string.
  ///
  /// \param RelativePath If non-null, will be set to the path relative to
  /// SearchPath at which the file was found. This only differs from the
  /// Filename for framework includes.
  ///
  /// \param SuggestedModule If non-null, and the file found is semantically
  /// part of a known module, this will be set to the module that should
  /// be imported instead of preprocessing/parsing the file found.
  const FileEntry *LookupFile(StringRef Filename, bool isAngled,
                              const DirectoryLookup *FromDir,
                              const DirectoryLookup *&CurDir,
                              const FileEntry *CurFileEnt,
                              SmallVectorImpl<char> *SearchPath,
                              SmallVectorImpl<char> *RelativePath,
                              Module **SuggestedModule,
                              bool SkipCache = false);

  /// LookupSubframeworkHeader - Look up a subframework for the specified
  /// #include file.  For example, if #include'ing <HIToolbox/HIToolbox.h> from
  /// within ".../Carbon.framework/Headers/Carbon.h", check to see if HIToolbox
  /// is a subframework within Carbon.framework.  If so, return the FileEntry
  /// for the designated file, otherwise return null.
  const FileEntry *LookupSubframeworkHeader(
      StringRef Filename,
      const FileEntry *RelativeFileEnt,
      SmallVectorImpl<char> *SearchPath,
      SmallVectorImpl<char> *RelativePath);

  /// LookupFrameworkCache - Look up the specified framework name in our
  /// framework cache, returning the DirectoryEntry it is in if we know,
  /// otherwise, return null.
  const DirectoryEntry *&LookupFrameworkCache(StringRef FWName) {
    return FrameworkMap.GetOrCreateValue(FWName).getValue();
  }

  /// ShouldEnterIncludeFile - Mark the specified file as a target of of a
  /// #include, #include_next, or #import directive.  Return false if #including
  /// the file will have no effect or true if we should include it.
  bool ShouldEnterIncludeFile(const FileEntry *File, bool isImport);


  /// getFileDirFlavor - Return whether the specified file is a normal header,
  /// a system header, or a C++ friendly system header.
  SrcMgr::CharacteristicKind getFileDirFlavor(const FileEntry *File) {
    return (SrcMgr::CharacteristicKind)getFileInfo(File).DirInfo;
  }

  /// MarkFileIncludeOnce - Mark the specified file as a "once only" file, e.g.
  /// due to #pragma once.
  void MarkFileIncludeOnce(const FileEntry *File) {
    HeaderFileInfo &FI = getFileInfo(File);
    FI.isImport = true;
    FI.isPragmaOnce = true;
  }

  /// MarkFileSystemHeader - Mark the specified file as a system header, e.g.
  /// due to #pragma GCC system_header.
  void MarkFileSystemHeader(const FileEntry *File) {
    getFileInfo(File).DirInfo = SrcMgr::C_System;
  }

  /// IncrementIncludeCount - Increment the count for the number of times the
  /// specified FileEntry has been entered.
  void IncrementIncludeCount(const FileEntry *File) {
    ++getFileInfo(File).NumIncludes;
  }

  /// SetFileControllingMacro - Mark the specified file as having a controlling
  /// macro.  This is used by the multiple-include optimization to eliminate
  /// no-op #includes.
  void SetFileControllingMacro(const FileEntry *File,
                               const IdentifierInfo *ControllingMacro) {
    getFileInfo(File).ControllingMacro = ControllingMacro;
  }

  /// \brief Determine whether this file is intended to be safe from
  /// multiple inclusions, e.g., it has #pragma once or a controlling
  /// macro.
  ///
  /// This routine does not consider the effect of #import 
  bool isFileMultipleIncludeGuarded(const FileEntry *File);

  /// CreateHeaderMap - This method returns a HeaderMap for the specified
  /// FileEntry, uniquing them through the the 'HeaderMaps' datastructure.
  const HeaderMap *CreateHeaderMap(const FileEntry *FE);

  /// \brief Search in the module cache path for a module with the given
  /// name.
  ///
  /// \param Module The module that was found with the given name, which 
  /// describes the module and how to build it.
  ///
  /// \param If non-NULL, will be set to the module file name we expected to
  /// find (regardless of whether it was actually found or not).
  ///
  /// \returns A file describing the named module, if already available in the
  /// cases, or NULL to indicate that the module could not be found.
  const FileEntry *lookupModule(StringRef ModuleName,
                                Module *&Module,
                                std::string *ModuleFileName = 0);
  
  void IncrementFrameworkLookupCount() { ++NumFrameworkLookups; }

  /// \brief Determine whether there is a module map that may map the header
  /// with the given file name to a (sub)module.
  ///
  /// \param Filename The name of the file.
  ///
  /// \param Root The "root" directory, at which we should stop looking for
  /// module maps.
  bool hasModuleMap(StringRef Filename, const DirectoryEntry *Root);
  
  /// \brief Retrieve the module that corresponds to the given file, if any.
  Module *findModuleForHeader(const FileEntry *File);
  
  
  /// \brief Read the contents of the given module map file.
  ///
  /// \param File The module map file.
  ///
  /// \param OnlyModule If non-NULL, this will receive the 
  ///
  /// \returns true if an error occurred, false otherwise.
  bool loadModuleMapFile(const FileEntry *File);
  
  /// \brief Retrieve a module with the given name.
  ///
  /// \param Name The name of the module to retrieve.
  ///
  /// \param AllowSearch If true, we're allowed to look for module maps within
  /// the header search path. Otherwise, the module must already be known.
  ///
  /// \returns The module, if found; otherwise, null.
  Module *getModule(StringRef Name, bool AllowSearch = true);

  /// \brief Retrieve a module with the given name, which may be part of the
  /// given framework.
  ///
  /// \param Name The name of the module to retrieve.
  ///
  /// \param Dir The framework directory (e.g., ModuleName.framework).
  ///
  /// \returns The module, if found; otherwise, null.
  Module *getFrameworkModule(StringRef Name, 
                                        const DirectoryEntry *Dir);

  /// \brief Retrieve the module map.
  ModuleMap &getModuleMap() { return ModMap; }
  
  unsigned header_file_size() const { return FileInfo.size(); }

  // Used by ASTReader.
  void setHeaderFileInfoForUID(HeaderFileInfo HFI, unsigned UID);

  /// getFileInfo - Return the HeaderFileInfo structure for the specified
  /// FileEntry.
  const HeaderFileInfo &getFileInfo(const FileEntry *FE) const {
    return const_cast<HeaderSearch*>(this)->getFileInfo(FE);
  }

  // Used by external tools
  typedef std::vector<DirectoryLookup>::const_iterator search_dir_iterator;
  search_dir_iterator search_dir_begin() const { return SearchDirs.begin(); }
  search_dir_iterator search_dir_end() const { return SearchDirs.end(); }
  unsigned search_dir_size() const { return SearchDirs.size(); }

  search_dir_iterator quoted_dir_begin() const {
    return SearchDirs.begin();
  }
  search_dir_iterator quoted_dir_end() const {
    return SearchDirs.begin() + AngledDirIdx;
  }

  search_dir_iterator angled_dir_begin() const {
    return SearchDirs.begin() + AngledDirIdx;
  }
  search_dir_iterator angled_dir_end() const {
    return SearchDirs.begin() + SystemDirIdx;
  }

  search_dir_iterator system_dir_begin() const {
    return SearchDirs.begin() + SystemDirIdx;
  }
  search_dir_iterator system_dir_end() const { return SearchDirs.end(); }

  /// \brief Retrieve a uniqued framework name.
  StringRef getUniqueFrameworkName(StringRef Framework);
  
  void PrintStats();
  
  size_t getTotalMemory() const;

  static std::string NormalizeDashIncludePath(StringRef File,
                                              FileManager &FileMgr);

private:
  /// \brief Describes what happened when we tried to load a module map file.
  enum LoadModuleMapResult {
    /// \brief The module map file had already been loaded.
    LMM_AlreadyLoaded,
    /// \brief The module map file was loaded by this invocation.
    LMM_NewlyLoaded,
    /// \brief There is was directory with the given name.
    LMM_NoDirectory,
    /// \brief There was either no module map file or the module map file was
    /// invalid.
    LMM_InvalidModuleMap
  };
  
  /// \brief Try to load the module map file in the given directory.
  ///
  /// \param DirName The name of the directory where we will look for a module
  /// map file.
  ///
  /// \returns The result of attempting to load the module map file from the
  /// named directory.
  LoadModuleMapResult loadModuleMapFile(StringRef DirName);

  /// \brief Try to load the module map file in the given directory.
  ///
  /// \param Dir The directory where we will look for a module map file.
  ///
  /// \returns The result of attempting to load the module map file from the
  /// named directory.
  LoadModuleMapResult loadModuleMapFile(const DirectoryEntry *Dir);

  /// getFileInfo - Return the HeaderFileInfo structure for the specified
  /// FileEntry.
  HeaderFileInfo &getFileInfo(const FileEntry *FE);
};

}  // end namespace clang

#endif
