//===--- SerializedDiagnosticPrinter.cpp - Serializer for diagnostics -----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include <vector>
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/DenseSet.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/Version.h"
#include "clang/Lex/Lexer.h"
#include "clang/Frontend/SerializedDiagnosticPrinter.h"

using namespace clang;
using namespace clang::serialized_diags;

namespace {
  
class AbbreviationMap {
  llvm::DenseMap<unsigned, unsigned> Abbrevs;
public:
  AbbreviationMap() {}
  
  void set(unsigned recordID, unsigned abbrevID) {
    assert(Abbrevs.find(recordID) == Abbrevs.end() 
           && "Abbreviation already set.");
    Abbrevs[recordID] = abbrevID;
  }
  
  unsigned get(unsigned recordID) {
    assert(Abbrevs.find(recordID) != Abbrevs.end() &&
           "Abbreviation not set.");
    return Abbrevs[recordID];
  }
};
 
typedef llvm::SmallVector<uint64_t, 64> RecordData;
typedef llvm::SmallVectorImpl<uint64_t> RecordDataImpl;
  
class SDiagsWriter : public DiagnosticConsumer {
public:  
  explicit SDiagsWriter(llvm::raw_ostream *os) 
    : LangOpts(0), Stream(Buffer), OS(os), inNonNoteDiagnostic(false)
  { 
    EmitPreamble();
  }
  
  ~SDiagsWriter() {}
  
  void HandleDiagnostic(DiagnosticsEngine::Level DiagLevel,
                        const Diagnostic &Info);
  
  void BeginSourceFile(const LangOptions &LO,
                       const Preprocessor *PP) {
    LangOpts = &LO;
  }

  virtual void finish();

  DiagnosticConsumer *clone(DiagnosticsEngine &Diags) const {
    // It makes no sense to clone this.
    return 0;
  }

private:
  /// \brief Emit the preamble for the serialized diagnostics.
  void EmitPreamble();
  
  /// \brief Emit the BLOCKINFO block.
  void EmitBlockInfoBlock();

  /// \brief Emit the META data block.
  void EmitMetaBlock();
  
  /// \brief Emit a record for a CharSourceRange.
  void EmitCharSourceRange(CharSourceRange R, SourceManager &SM);
  
  /// \brief Emit the string information for the category for a diagnostic.
  unsigned getEmitCategory(unsigned DiagID);
  
  /// \brief Emit the string information for diagnostic flags.
  unsigned getEmitDiagnosticFlag(DiagnosticsEngine::Level DiagLevel,
                                 const Diagnostic &Info);
  
  /// \brief Emit (lazily) the file string and retrieved the file identifier.
  unsigned getEmitFile(SourceLocation Loc, SourceManager &SM);
  
  /// \brief Add SourceLocation information the specified record.
  void AddLocToRecord(SourceLocation Loc, RecordDataImpl &Record,
                      SourceManager &SM,
                      unsigned TokSize = 0);

  /// \brief Add CharSourceRange information the specified record.
  void AddCharSourceRangeToRecord(CharSourceRange R, RecordDataImpl &Record,
                                  SourceManager &SM);

  /// \brief The version of the diagnostics file.
  enum { Version = 1 };

  const LangOptions *LangOpts;
  
  /// \brief The byte buffer for the serialized content.
  std::vector<unsigned char> Buffer;

  /// \brief The BitStreamWriter for the serialized diagnostics.
  llvm::BitstreamWriter Stream;

  /// \brief The name of the diagnostics file.
  llvm::OwningPtr<llvm::raw_ostream> OS;
  
  /// \brief The set of constructed record abbreviations.
  AbbreviationMap Abbrevs;

  /// \brief A utility buffer for constructing record content.
  RecordData Record;

  /// \brief A text buffer for rendering diagnostic text.
  llvm::SmallString<256> diagBuf;
  
  /// \brief The collection of diagnostic categories used.
  llvm::DenseSet<unsigned> Categories;
  
  /// \brief The collection of files used.
  llvm::DenseMap<const FileEntry *, unsigned> Files;

  typedef llvm::DenseMap<const void *, std::pair<unsigned, llvm::StringRef> > 
          DiagFlagsTy;

  /// \brief Map for uniquing strings.
  DiagFlagsTy DiagFlags;
  
  /// \brief Flag indicating whether or not we are in the process of
  /// emitting a non-note diagnostic.
  bool inNonNoteDiagnostic;
};
} // end anonymous namespace

namespace clang {
namespace serialized_diags {
DiagnosticConsumer *create(llvm::raw_ostream *OS) {
  return new SDiagsWriter(OS);
}
} // end namespace serialized_diags
} // end namespace clang

//===----------------------------------------------------------------------===//
// Serialization methods.
//===----------------------------------------------------------------------===//

/// \brief Emits a block ID in the BLOCKINFO block.
static void EmitBlockID(unsigned ID, const char *Name,
                        llvm::BitstreamWriter &Stream,
                        RecordDataImpl &Record) {
  Record.clear();
  Record.push_back(ID);
  Stream.EmitRecord(llvm::bitc::BLOCKINFO_CODE_SETBID, Record);
  
  // Emit the block name if present.
  if (Name == 0 || Name[0] == 0)
    return;

  Record.clear();

  while (*Name)
    Record.push_back(*Name++);

  Stream.EmitRecord(llvm::bitc::BLOCKINFO_CODE_BLOCKNAME, Record);
}

/// \brief Emits a record ID in the BLOCKINFO block.
static void EmitRecordID(unsigned ID, const char *Name,
                         llvm::BitstreamWriter &Stream,
                         RecordDataImpl &Record){
  Record.clear();
  Record.push_back(ID);

  while (*Name)
    Record.push_back(*Name++);

  Stream.EmitRecord(llvm::bitc::BLOCKINFO_CODE_SETRECORDNAME, Record);
}

void SDiagsWriter::AddLocToRecord(SourceLocation Loc,
                                  RecordDataImpl &Record,
                                  SourceManager &SM,
                                  unsigned TokSize) {
  if (Loc.isInvalid()) {
    // Emit a "sentinel" location.
    Record.push_back((unsigned)0); // File.
    Record.push_back((unsigned)0); // Line.
    Record.push_back((unsigned)0); // Column.
    Record.push_back((unsigned)0); // Offset.
    return;
  }

  Loc = SM.getSpellingLoc(Loc);
  Record.push_back(getEmitFile(Loc, SM));
  Record.push_back(SM.getSpellingLineNumber(Loc));
  Record.push_back(SM.getSpellingColumnNumber(Loc)+TokSize);
  Record.push_back(SM.getFileOffset(Loc));
}

void SDiagsWriter::AddCharSourceRangeToRecord(CharSourceRange Range,
                                              RecordDataImpl &Record,
                                              SourceManager &SM) {
  AddLocToRecord(Range.getBegin(), Record, SM);
  unsigned TokSize = 0;
  if (Range.isTokenRange())
    TokSize = Lexer::MeasureTokenLength(Range.getEnd(),
                                        SM, *LangOpts);
  
  AddLocToRecord(Range.getEnd(), Record, SM, TokSize);
}

unsigned SDiagsWriter::getEmitFile(SourceLocation Loc, SourceManager &SM) {
  assert(Loc.isValid());
  const std::pair<FileID, unsigned> &LocInfo = SM.getDecomposedLoc(Loc);
  const FileEntry *FE = SM.getFileEntryForID(LocInfo.first);
  if (!FE)
    return 0;
  
  unsigned &entry = Files[FE];
  if (entry)
    return entry;
  
  // Lazily generate the record for the file.
  entry = Files.size();
  RecordData Record;
  Record.push_back(RECORD_FILENAME);
  Record.push_back(entry);
  Record.push_back(FE->getSize());
  Record.push_back(FE->getModificationTime());
  StringRef Name = FE->getName();
  Record.push_back(Name.size());
  Stream.EmitRecordWithBlob(Abbrevs.get(RECORD_FILENAME), Record, Name);

  return entry;
}

void SDiagsWriter::EmitCharSourceRange(CharSourceRange R,
                                       SourceManager &SM) {
  Record.clear();
  Record.push_back(RECORD_SOURCE_RANGE);
  AddCharSourceRangeToRecord(R, Record, SM);
  Stream.EmitRecordWithAbbrev(Abbrevs.get(RECORD_SOURCE_RANGE), Record);
}

/// \brief Emits the preamble of the diagnostics file.
void SDiagsWriter::EmitPreamble() {
  // Emit the file header.
  Stream.Emit((unsigned)'D', 8);
  Stream.Emit((unsigned)'I', 8);
  Stream.Emit((unsigned)'A', 8);
  Stream.Emit((unsigned)'G', 8);

  EmitBlockInfoBlock();
  EmitMetaBlock();
}

static void AddSourceLocationAbbrev(llvm::BitCodeAbbrev *Abbrev) {
  using namespace llvm;
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 10)); // File ID.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 32)); // Line.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 32)); // Column.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 32)); // Offset;
}

static void AddRangeLocationAbbrev(llvm::BitCodeAbbrev *Abbrev) {
  AddSourceLocationAbbrev(Abbrev);
  AddSourceLocationAbbrev(Abbrev);  
}

void SDiagsWriter::EmitBlockInfoBlock() {
  Stream.EnterBlockInfoBlock(3);

  using namespace llvm;

  // ==---------------------------------------------------------------------==//
  // The subsequent records and Abbrevs are for the "Meta" block.
  // ==---------------------------------------------------------------------==//

  EmitBlockID(BLOCK_META, "Meta", Stream, Record);
  EmitRecordID(RECORD_VERSION, "Version", Stream, Record);
  BitCodeAbbrev *Abbrev = new BitCodeAbbrev();
  Abbrev->Add(BitCodeAbbrevOp(RECORD_VERSION));
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 32));
  Abbrevs.set(RECORD_VERSION, Stream.EmitBlockInfoAbbrev(BLOCK_META, Abbrev));

  // ==---------------------------------------------------------------------==//
  // The subsequent records and Abbrevs are for the "Diagnostic" block.
  // ==---------------------------------------------------------------------==//

  EmitBlockID(BLOCK_DIAG, "Diag", Stream, Record);
  EmitRecordID(RECORD_DIAG, "DiagInfo", Stream, Record);
  EmitRecordID(RECORD_SOURCE_RANGE, "SrcRange", Stream, Record);
  EmitRecordID(RECORD_CATEGORY, "CatName", Stream, Record);
  EmitRecordID(RECORD_DIAG_FLAG, "DiagFlag", Stream, Record);
  EmitRecordID(RECORD_FILENAME, "FileName", Stream, Record);
  EmitRecordID(RECORD_FIXIT, "FixIt", Stream, Record);

  // Emit abbreviation for RECORD_DIAG.
  Abbrev = new BitCodeAbbrev();
  Abbrev->Add(BitCodeAbbrevOp(RECORD_DIAG));
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 3));  // Diag level.
  AddSourceLocationAbbrev(Abbrev);
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 10)); // Category.  
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 10)); // Mapped Diag ID.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 16)); // Text size.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Blob)); // Diagnostc text.
  Abbrevs.set(RECORD_DIAG, Stream.EmitBlockInfoAbbrev(BLOCK_DIAG, Abbrev));
  
  // Emit abbrevation for RECORD_CATEGORY.
  Abbrev = new BitCodeAbbrev();
  Abbrev->Add(BitCodeAbbrevOp(RECORD_CATEGORY));
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 16)); // Category ID.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 8));  // Text size.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Blob));      // Category text.
  Abbrevs.set(RECORD_CATEGORY, Stream.EmitBlockInfoAbbrev(BLOCK_DIAG, Abbrev));

  // Emit abbrevation for RECORD_SOURCE_RANGE.
  Abbrev = new BitCodeAbbrev();
  Abbrev->Add(BitCodeAbbrevOp(RECORD_SOURCE_RANGE));
  AddRangeLocationAbbrev(Abbrev);
  Abbrevs.set(RECORD_SOURCE_RANGE,
              Stream.EmitBlockInfoAbbrev(BLOCK_DIAG, Abbrev));
  
  // Emit the abbreviation for RECORD_DIAG_FLAG.
  Abbrev = new BitCodeAbbrev();
  Abbrev->Add(BitCodeAbbrevOp(RECORD_DIAG_FLAG));
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 10)); // Mapped Diag ID.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 16)); // Text size.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Blob)); // Flag name text.
  Abbrevs.set(RECORD_DIAG_FLAG, Stream.EmitBlockInfoAbbrev(BLOCK_DIAG,
                                                           Abbrev));
  
  // Emit the abbreviation for RECORD_FILENAME.
  Abbrev = new BitCodeAbbrev();
  Abbrev->Add(BitCodeAbbrevOp(RECORD_FILENAME));
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 10)); // Mapped file ID.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 32)); // Size.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 32)); // Modifcation time.  
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 16)); // Text size.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Blob)); // File name text.
  Abbrevs.set(RECORD_FILENAME, Stream.EmitBlockInfoAbbrev(BLOCK_DIAG,
                                                          Abbrev));
  
  // Emit the abbreviation for RECORD_FIXIT.
  Abbrev = new BitCodeAbbrev();
  Abbrev->Add(BitCodeAbbrevOp(RECORD_FIXIT));
  AddRangeLocationAbbrev(Abbrev);
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 16)); // Text size.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Blob));      // FixIt text.
  Abbrevs.set(RECORD_FIXIT, Stream.EmitBlockInfoAbbrev(BLOCK_DIAG,
                                                       Abbrev));

  Stream.ExitBlock();
}

void SDiagsWriter::EmitMetaBlock() {
  Stream.EnterSubblock(BLOCK_META, 3);
  Record.clear();
  Record.push_back(RECORD_VERSION);
  Record.push_back(Version);
  Stream.EmitRecordWithAbbrev(Abbrevs.get(RECORD_VERSION), Record);  
  Stream.ExitBlock();
}

unsigned SDiagsWriter::getEmitCategory(unsigned int DiagID) {
  unsigned category = DiagnosticIDs::getCategoryNumberForDiag(DiagID);
  
  if (Categories.count(category))
    return category;
  
  Categories.insert(category);
  
  // We use a local version of 'Record' so that we can be generating
  // another record when we lazily generate one for the category entry.
  RecordData Record;
  Record.push_back(RECORD_CATEGORY);
  Record.push_back(category);
  StringRef catName = DiagnosticIDs::getCategoryNameFromID(category);
  Record.push_back(catName.size());
  Stream.EmitRecordWithBlob(Abbrevs.get(RECORD_CATEGORY), Record, catName);
  
  return category;
}

unsigned SDiagsWriter::getEmitDiagnosticFlag(DiagnosticsEngine::Level DiagLevel,
                                             const Diagnostic &Info) {
  if (DiagLevel == DiagnosticsEngine::Note)
    return 0; // No flag for notes.
  
  StringRef FlagName = DiagnosticIDs::getWarningOptionForDiag(Info.getID());
  if (FlagName.empty())
    return 0;

  // Here we assume that FlagName points to static data whose pointer
  // value is fixed.  This allows us to unique by diagnostic groups.
  const void *data = FlagName.data();
  std::pair<unsigned, StringRef> &entry = DiagFlags[data];
  if (entry.first == 0) {
    entry.first = DiagFlags.size();
    entry.second = FlagName;
    
    // Lazily emit the string in a separate record.
    RecordData Record;
    Record.push_back(RECORD_DIAG_FLAG);
    Record.push_back(entry.first);
    Record.push_back(FlagName.size());
    Stream.EmitRecordWithBlob(Abbrevs.get(RECORD_DIAG_FLAG),
                              Record, FlagName);    
  }

  return entry.first;
}

void SDiagsWriter::HandleDiagnostic(DiagnosticsEngine::Level DiagLevel,
                                    const Diagnostic &Info) {
  SourceManager &SM = Info.getSourceManager();

  if (DiagLevel != DiagnosticsEngine::Note) {
    if (inNonNoteDiagnostic) {
      // We have encountered a non-note diagnostic.  Finish up the previous
      // diagnostic block before starting a new one.
      Stream.ExitBlock();
    }
    inNonNoteDiagnostic = true;
  }
  
  Stream.EnterSubblock(BLOCK_DIAG, 4);
  
  // Emit the RECORD_DIAG record.
  Record.clear();
  Record.push_back(RECORD_DIAG);
  Record.push_back(DiagLevel);
  AddLocToRecord(Info.getLocation(), Record, SM);
  // Emit the category string lazily and get the category ID.
  Record.push_back(getEmitCategory(Info.getID()));
  // Emit the diagnostic flag string lazily and get the mapped ID.
  Record.push_back(getEmitDiagnosticFlag(DiagLevel, Info));
  // Emit the diagnostic text.
  diagBuf.clear();   
  Info.FormatDiagnostic(diagBuf); // Compute the diagnostic text.
  Record.push_back(diagBuf.str().size());
  Stream.EmitRecordWithBlob(Abbrevs.get(RECORD_DIAG), Record, diagBuf.str());

  // Emit Source Ranges.
  ArrayRef<CharSourceRange> Ranges = Info.getRanges();
  for (ArrayRef<CharSourceRange>::iterator it=Ranges.begin(), ei=Ranges.end();
       it != ei; ++it) {
    if (it->isValid())
      EmitCharSourceRange(*it, SM);
  }

  // Emit FixIts.
  for (unsigned i = 0, n = Info.getNumFixItHints(); i != n; ++i) {
    const FixItHint &fix = Info.getFixItHint(i);
    if (fix.isNull())
      continue;
    Record.clear();
    Record.push_back(RECORD_FIXIT);
    AddCharSourceRangeToRecord(fix.RemoveRange, Record, SM);
    Record.push_back(fix.CodeToInsert.size());
    Stream.EmitRecordWithBlob(Abbrevs.get(RECORD_FIXIT), Record,
                              fix.CodeToInsert);    
  }
  
  if (DiagLevel == DiagnosticsEngine::Note) {
    // Notes currently cannot have child diagnostics.  Complete the
    // diagnostic now.
    Stream.ExitBlock();
  }
}

void SDiagsWriter::finish() {
  if (inNonNoteDiagnostic) {
    // Finish off any diagnostics we were in the process of emitting.
    Stream.ExitBlock();
    inNonNoteDiagnostic = false;
  }

  // Write the generated bitstream to "Out".
  OS->write((char *)&Buffer.front(), Buffer.size());
  OS->flush();
  
  OS.reset(0);
}

