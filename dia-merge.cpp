// Copyright (c) 2022 Bartosz Polaczyk
//
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.


#include "clang/Frontend/DiagnosticRenderer.h"
#include "clang/Frontend/SerializedDiagnosticReader.h"
#include "clang/Lex/Lexer.h"
#include "clang/Frontend/SerializedDiagnostics.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "llvm/Bitstream/BitstreamWriter.h"
#include "llvm/Support/CommandLine.h"

#include <regex>

using namespace llvm;
using namespace clang;

static cl::list<std::string> InputFilename(cl::Positional, cl::OneOrMore,
                                          cl::desc("<input .dia>")
                                          );

static cl::opt<std::string> OutputFilename("output",
                                             cl::desc("output="), cl::Required);

static cl::list<std::string> PathRemaps("remap",
                                        cl::desc("Path remapping substitution"),
                                        cl::value_desc("regex=replacement"));
static cl::alias PathRemapsAlias("r", cl::aliasopt(PathRemaps));

static cl::opt<bool> Stream("stream", cl::desc("Read the dia from the fifo file. Requires exactly one input path"), cl::init(false));
static cl::alias StreamAlias("s", cl::aliasopt(Stream));

// Remapper inspired by https://github.com/MobileNativeFoundation/index-import

struct Remapper {
public:
  std::string remap(const llvm::StringRef input) const {
    std::string input_str = input.str();
    for (const auto &remap : this->_remaps) {
      const auto &pattern = std::get<std::regex>(remap);
      const auto &replacement = std::get<std::string>(remap);

      std::smatch match;
      if (std::regex_search(input_str, match, pattern)) {
        // Replace the regex multiple times
        auto substitution = match.format(replacement);
        input_str = match.prefix().str() + substitution + match.suffix().str();
      }
    }

    return input_str;
  }

  void addRemap(const std::regex &pattern, const std::string &replacement) {
    this->_remaps.emplace_back(pattern, replacement);
  }

  std::vector<std::pair<std::regex, std::string>> _remaps;
};

// Based on LLVM's BitcodeWriter.h, SerializedDiagnosticPrinter.cpp
// and other files in https://github.com/llvm/llvm-project/

typedef SmallVector<uint64_t, 64> RecordData;
typedef SmallVectorImpl<uint64_t> RecordDataImpl;
typedef ArrayRef<uint64_t> RecordDataRef;
typedef llvm::DenseMap<unsigned, unsigned> AbbrevLookup;


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

class SDiagsWriter : public DiagnosticConsumer {
  friend class SDiagsRenderer;
  friend class SDiagsMerger;

  struct SharedState;

  explicit SDiagsWriter(std::shared_ptr<SharedState> State)
      : LangOpts(nullptr), OriginalInstance(false), MergeChildRecords(false),
        State(std::move(State)) {}

public:
  SDiagsWriter(StringRef File, DiagnosticOptions *Diags, bool MergeChildRecords, Remapper remapper)
      : LangOpts(nullptr), OriginalInstance(true),
        MergeChildRecords(MergeChildRecords),
      PathsRemappper(std::make_shared<Remapper>(remapper)),
        State(std::make_shared<SharedState>(File, Diags))
               {
    if (MergeChildRecords)
      RemoveOldDiagnostics();
    EmitPreamble();
  }

  ~SDiagsWriter() override {}

  void HandleDiagnostic(DiagnosticsEngine::Level DiagLevel,
                        const Diagnostic &Info) override;

  void BeginSourceFile(const LangOptions &LO, const Preprocessor *PP) override {
    LangOpts = &LO;
  }

  void finish() override;

private:
  /// Build a DiagnosticsEngine to emit diagnostics about the diagnostics
  DiagnosticsEngine *getMetaDiags();


  /// Remove old copies of the serialized diagnostics. This is necessary
  /// so that we can detect when subprocesses write diagnostics that we should
  /// merge into our own.
  void RemoveOldDiagnostics();

  /// Emit the preamble for the serialized diagnostics.
  void EmitPreamble();

  /// Emit the BLOCKINFO block.
  void EmitBlockInfoBlock();

  /// Emit the META data block.
  void EmitMetaBlock();

  /// Start a DIAG block.
  void EnterDiagBlock();

  /// End a DIAG block.
  void ExitDiagBlock();

  /// Emit a DIAG record.
  void EmitDiagnosticMessage(FullSourceLoc Loc, PresumedLoc PLoc,
                             DiagnosticsEngine::Level Level, StringRef Message,
                             DiagOrStoredDiag D);

  /// Emit FIXIT and SOURCE_RANGE records for a diagnostic.
  void EmitCodeContext(SmallVectorImpl<CharSourceRange> &Ranges,
                       ArrayRef<FixItHint> Hints,
                       const SourceManager &SM);

  /// Emit a record for a CharSourceRange.
  void EmitCharSourceRange(CharSourceRange R, const SourceManager &SM);

  /// Emit the string information for the category.
  unsigned getEmitCategory(unsigned category = 0);

  /// Emit the string information for diagnostic flags.
  unsigned getEmitDiagnosticFlag(DiagnosticsEngine::Level DiagLevel,
                                 unsigned DiagID = 0);

  unsigned getEmitDiagnosticFlag(StringRef DiagName);

  /// Emit (lazily) the file string and retrieved the file identifier.
  unsigned getEmitFile(const char *Filename);

  /// Add SourceLocation information the specified record.
  void AddLocToRecord(FullSourceLoc Loc, PresumedLoc PLoc,
                      RecordDataImpl &Record, unsigned TokSize = 0);

  /// Add SourceLocation information the specified record.
  void AddLocToRecord(FullSourceLoc Loc, RecordDataImpl &Record,
                      unsigned TokSize = 0) {
    AddLocToRecord(Loc, Loc.hasManager() ? Loc.getPresumedLoc() : PresumedLoc(),
                   Record, TokSize);
  }

  /// Add CharSourceRange information the specified record.
  void AddCharSourceRangeToRecord(CharSourceRange R, RecordDataImpl &Record,
                                  const SourceManager &SM);

  /// Language options, which can differ from one clone of this client
  /// to another.
  const LangOptions *LangOpts;

  /// Whether this is the original instance (rather than one of its
  /// clones), responsible for writing the file at the end.
  bool OriginalInstance;

  /// Whether this instance should aggregate diagnostics that are
  /// generated from child processes.
  bool MergeChildRecords;

  /// Whether we've started finishing and tearing down this instance.
  bool IsFinishing = false;
    
    std::shared_ptr<Remapper> PathsRemappper;

  /// State that is shared among the various clones of this diagnostic
  /// consumer.
  struct SharedState {
    SharedState(StringRef File, DiagnosticOptions *Diags)
        : DiagOpts(Diags), Stream(Buffer), OutputFile(File.str()),
          EmittedAnyDiagBlocks(false) {}

    /// Diagnostic options.
    IntrusiveRefCntPtr<DiagnosticOptions> DiagOpts;

    /// The byte buffer for the serialized content.
    SmallString<1024> Buffer;

    /// The BitStreamWriter for the serialized diagnostics.
    llvm::BitstreamWriter Stream;

    /// The name of the diagnostics file.
    std::string OutputFile;

    /// The set of constructed record abbreviations.
    AbbreviationMap Abbrevs;

    /// A utility buffer for constructing record content.
    RecordData Record;

    /// A text buffer for rendering diagnostic text.
    SmallString<256> diagBuf;

    /// The collection of diagnostic categories used.
    llvm::DenseSet<unsigned> Categories;

    /// The collection of files used.
    llvm::DenseMap<const char *, unsigned> Files;

    typedef llvm::DenseMap<const void *, std::pair<unsigned, StringRef> >
    DiagFlagsTy;

    /// Map for uniquing strings.
    DiagFlagsTy DiagFlags;

    /// Whether we have already started emission of any DIAG blocks. Once
    /// this becomes \c true, we never close a DIAG block until we know that we're
    /// starting another one or we're done.
    bool EmittedAnyDiagBlocks;

    /// Engine for emitting diagnostics about the diagnostics.
    std::unique_ptr<DiagnosticsEngine> MetaDiagnostics;
  };

  /// State shared among the various clones of this diagnostic consumer.
  std::shared_ptr<SharedState> State;
};

static SDiagsWriter *sharedWriter;

class SDiagsMerger : clang::serialized_diags::SerializedDiagnosticReader {
  SDiagsWriter &Writer;
  AbbrevLookup FileLookup;
  AbbrevLookup CategoryLookup;
  AbbrevLookup DiagFlagLookup;

public:
  SDiagsMerger(SDiagsWriter &Writer)
      : SerializedDiagnosticReader(), Writer(Writer) {}

  std::error_code mergeRecordsFromFile(const char *File) {
    return readDiagnostics(File);
  }

protected:
  std::error_code visitStartOfDiagnostic() override;
  std::error_code visitEndOfDiagnostic() override;
  std::error_code visitCategoryRecord(unsigned ID, StringRef Name) override;
  std::error_code visitDiagFlagRecord(unsigned ID, StringRef Name) override;
  std::error_code visitDiagnosticRecord(
      unsigned Severity, const serialized_diags::Location &Location,
      unsigned Category, unsigned Flag, StringRef Message) override;
  std::error_code visitFilenameRecord(unsigned ID, unsigned Size,
                                      unsigned Timestamp,
                                      StringRef Name) override;
  std::error_code visitFixitRecord(const serialized_diags::Location &Start,
                                   const serialized_diags::Location &End,
                                   StringRef CodeToInsert) override;
  std::error_code
  visitSourceRangeRecord(const serialized_diags::Location &Start,
                         const serialized_diags::Location &End) override;

private:
  std::error_code adjustSourceLocFilename(RecordData &Record,
                                          unsigned int offset);

  void adjustAbbrevID(RecordData &Record, AbbrevLookup &Lookup,
                      unsigned NewAbbrev);

  void writeRecordWithAbbrev(unsigned ID, RecordData &Record);

  void writeRecordWithBlob(unsigned ID, RecordData &Record, StringRef Blob);
};

class SDiagsRenderer : public DiagnosticNoteRenderer {
  SDiagsWriter &Writer;
public:
  SDiagsRenderer(SDiagsWriter &Writer, const LangOptions &LangOpts,
                 DiagnosticOptions *DiagOpts)
    : DiagnosticNoteRenderer(LangOpts, DiagOpts), Writer(Writer) {}

  ~SDiagsRenderer() override {}

protected:
  void emitDiagnosticMessage(FullSourceLoc Loc, PresumedLoc PLoc,
                             DiagnosticsEngine::Level Level, StringRef Message,
                             ArrayRef<CharSourceRange> Ranges,
                             DiagOrStoredDiag D) override;

  void emitDiagnosticLoc(FullSourceLoc Loc, PresumedLoc PLoc,
                         DiagnosticsEngine::Level Level,
                         ArrayRef<CharSourceRange> Ranges) override {}

  void emitNote(FullSourceLoc Loc, StringRef Message) override;

  void emitCodeContext(FullSourceLoc Loc, DiagnosticsEngine::Level Level,
                       SmallVectorImpl<CharSourceRange> &Ranges,
                       ArrayRef<FixItHint> Hints) override;

  void beginDiagnostic(DiagOrStoredDiag D,
                       DiagnosticsEngine::Level Level) override;
  void endDiagnostic(DiagOrStoredDiag D,
                     DiagnosticsEngine::Level Level) override;
};


/// Emits a block ID in the BLOCKINFO block.
static void EmitBlockID(unsigned ID, const char *Name,
                        llvm::BitstreamWriter &Stream,
                        RecordDataImpl &Record) {
  Record.clear();
  Record.push_back(ID);
  Stream.EmitRecord(llvm::bitc::BLOCKINFO_CODE_SETBID, Record);

  // Emit the block name if present.
  if (!Name || Name[0] == 0)
    return;

  Record.clear();

  while (*Name)
    Record.push_back(*Name++);

  Stream.EmitRecord(llvm::bitc::BLOCKINFO_CODE_BLOCKNAME, Record);
}

/// Emits a record ID in the BLOCKINFO block.
static void EmitRecordID(unsigned ID, const char *Name,
                         llvm::BitstreamWriter &Stream,
                         RecordDataImpl &Record){
  Record.clear();
  Record.push_back(ID);

  while (*Name)
    Record.push_back(*Name++);

  Stream.EmitRecord(llvm::bitc::BLOCKINFO_CODE_SETRECORDNAME, Record);
}

void SDiagsWriter::AddLocToRecord(FullSourceLoc Loc, PresumedLoc PLoc,
                                  RecordDataImpl &Record, unsigned TokSize) {
  if (PLoc.isInvalid()) {
    // Emit a "sentinel" location.
    Record.push_back((unsigned)0); // File.
    Record.push_back((unsigned)0); // Line.
    Record.push_back((unsigned)0); // Column.
    Record.push_back((unsigned)0); // Offset.
    return;
  }

  Record.push_back(getEmitFile(PLoc.getFilename()));
  Record.push_back(PLoc.getLine());
  Record.push_back(PLoc.getColumn()+TokSize);
  Record.push_back(Loc.getFileOffset());
}

void SDiagsWriter::AddCharSourceRangeToRecord(CharSourceRange Range,
                                              RecordDataImpl &Record,
                                              const SourceManager &SM) {
  AddLocToRecord(FullSourceLoc(Range.getBegin(), SM), Record);
  unsigned TokSize = 0;
  if (Range.isTokenRange())
    TokSize = Lexer::MeasureTokenLength(Range.getEnd(),
                                        SM, *LangOpts);

  AddLocToRecord(FullSourceLoc(Range.getEnd(), SM), Record, TokSize);
}

unsigned SDiagsWriter::getEmitFile(const char *FileName){
  if (!FileName)
    return 0;

  StringRef Name(FileName);
  auto remapped = PathsRemappper->remap(Name);
  StringRef RemappedName(remapped);

  // TODO(polac24): Reuse file abbrevations - right now it always emits filepath
  // This workaround was added as Densemap returns values for not valid entries.
  State->Files.clear();
  unsigned &entry = State->Files[RemappedName.str().c_str()];
    if (entry) {
        return entry;
    }

  // Lazily generate the record for the file.
  entry = State->Files.size();
  RecordData::value_type Record[] = {clang::serialized_diags::RECORD_FILENAME, entry, 0 /* For legacy */,
                                     0 /* For legacy */, RemappedName.size()};
  State->Stream.EmitRecordWithBlob(State->Abbrevs.get(clang::serialized_diags::RECORD_FILENAME), Record,
                                     RemappedName);

  return entry;
}

void SDiagsWriter::EmitCharSourceRange(CharSourceRange R,
                                       const SourceManager &SM) {
  State->Record.clear();
  State->Record.push_back(clang::serialized_diags::RECORD_SOURCE_RANGE);
  AddCharSourceRangeToRecord(R, State->Record, SM);
  State->Stream.EmitRecordWithAbbrev(State->Abbrevs.get(clang::serialized_diags::RECORD_SOURCE_RANGE),
                                     State->Record);
}

/// Emits the preamble of the diagnostics file.
void SDiagsWriter::EmitPreamble() {
  // Emit the file header.
  State->Stream.Emit((unsigned)'D', 8);
  State->Stream.Emit((unsigned)'I', 8);
  State->Stream.Emit((unsigned)'A', 8);
  State->Stream.Emit((unsigned)'G', 8);

  EmitBlockInfoBlock();
  EmitMetaBlock();
}

static void AddSourceLocationAbbrev(llvm::BitCodeAbbrev &Abbrev) {
  using namespace llvm;
  Abbrev.Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 10)); // File ID.
  Abbrev.Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 32)); // Line.
  Abbrev.Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 32)); // Column.
  Abbrev.Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 32)); // Offset;
}

static void AddRangeLocationAbbrev(llvm::BitCodeAbbrev &Abbrev) {
  AddSourceLocationAbbrev(Abbrev);
  AddSourceLocationAbbrev(Abbrev);
}

void SDiagsWriter::EmitBlockInfoBlock() {
  State->Stream.EnterBlockInfoBlock();

  using namespace llvm;
  llvm::BitstreamWriter &Stream = State->Stream;
  RecordData &Record = State->Record;
  AbbreviationMap &Abbrevs = State->Abbrevs;

  // ==---------------------------------------------------------------------==//
  // The subsequent records and Abbrevs are for the "Meta" block.
  // ==---------------------------------------------------------------------==//

    EmitBlockID(clang::serialized_diags::BLOCK_META, "Meta", Stream, Record);
  EmitRecordID(clang::serialized_diags::RECORD_VERSION, "Version", Stream, Record);
  auto Abbrev = std::make_shared<BitCodeAbbrev>();
  Abbrev->Add(BitCodeAbbrevOp(clang::serialized_diags::RECORD_VERSION));
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 32));
  Abbrevs.set(clang::serialized_diags::RECORD_VERSION, Stream.EmitBlockInfoAbbrev(clang::serialized_diags::BLOCK_META, Abbrev));

  // ==---------------------------------------------------------------------==//
  // The subsequent records and Abbrevs are for the "Diagnostic" block.
  // ==---------------------------------------------------------------------==//

  EmitBlockID(clang::serialized_diags::BLOCK_DIAG, "Diag", Stream, Record);
  EmitRecordID(clang::serialized_diags::RECORD_DIAG, "DiagInfo", Stream, Record);
  EmitRecordID(clang::serialized_diags::RECORD_SOURCE_RANGE, "SrcRange", Stream, Record);
  EmitRecordID(clang::serialized_diags::RECORD_CATEGORY, "CatName", Stream, Record);
  EmitRecordID(clang::serialized_diags::RECORD_DIAG_FLAG, "DiagFlag", Stream, Record);
  EmitRecordID(clang::serialized_diags::RECORD_FILENAME, "FileName", Stream, Record);
  EmitRecordID(clang::serialized_diags::RECORD_FIXIT, "FixIt", Stream, Record);

  // Emit abbreviation for RECORD_DIAG.
  Abbrev = std::make_shared<BitCodeAbbrev>();
  Abbrev->Add(BitCodeAbbrevOp(clang::serialized_diags::RECORD_DIAG));
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 3));  // Diag level.
  AddSourceLocationAbbrev(*Abbrev);
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 10)); // Category.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 10)); // Mapped Diag ID.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 16)); // Text size.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Blob)); // Diagnostc text.
    Abbrevs.set(clang::serialized_diags::RECORD_DIAG, Stream.EmitBlockInfoAbbrev(clang::serialized_diags::BLOCK_DIAG, Abbrev));

  // Emit abbreviation for RECORD_CATEGORY.
  Abbrev = std::make_shared<BitCodeAbbrev>();
  Abbrev->Add(BitCodeAbbrevOp(clang::serialized_diags::RECORD_CATEGORY));
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 16)); // Category ID.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 8));  // Text size.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Blob));      // Category text.
  Abbrevs.set(clang::serialized_diags::RECORD_CATEGORY, Stream.EmitBlockInfoAbbrev(clang::serialized_diags::BLOCK_DIAG, Abbrev));

  // Emit abbreviation for RECORD_SOURCE_RANGE.
  Abbrev = std::make_shared<BitCodeAbbrev>();
  Abbrev->Add(BitCodeAbbrevOp(clang::serialized_diags::RECORD_SOURCE_RANGE));
  AddRangeLocationAbbrev(*Abbrev);
  Abbrevs.set(clang::serialized_diags::RECORD_SOURCE_RANGE,
              Stream.EmitBlockInfoAbbrev(clang::serialized_diags::BLOCK_DIAG, Abbrev));

  // Emit the abbreviation for RECORD_DIAG_FLAG.
  Abbrev = std::make_shared<BitCodeAbbrev>();
  Abbrev->Add(BitCodeAbbrevOp(clang::serialized_diags::RECORD_DIAG_FLAG));
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 10)); // Mapped Diag ID.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 16)); // Text size.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Blob)); // Flag name text.
  Abbrevs.set(clang::serialized_diags::RECORD_DIAG_FLAG, Stream.EmitBlockInfoAbbrev(clang::serialized_diags::BLOCK_DIAG,
                                                           Abbrev));

  // Emit the abbreviation for RECORD_FILENAME.
  Abbrev = std::make_shared<BitCodeAbbrev>();
  Abbrev->Add(BitCodeAbbrevOp(clang::serialized_diags::RECORD_FILENAME));
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 10)); // Mapped file ID.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 32)); // Size.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 32)); // Modification time.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 16)); // Text size.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Blob)); // File name text.
  Abbrevs.set(clang::serialized_diags::RECORD_FILENAME, Stream.EmitBlockInfoAbbrev(clang::serialized_diags::BLOCK_DIAG,
                                                          Abbrev));

  // Emit the abbreviation for RECORD_FIXIT.
  Abbrev = std::make_shared<BitCodeAbbrev>();
  Abbrev->Add(BitCodeAbbrevOp(clang::serialized_diags::RECORD_FIXIT));
  AddRangeLocationAbbrev(*Abbrev);
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 16)); // Text size.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Blob));      // FixIt text.
  Abbrevs.set(clang::serialized_diags::RECORD_FIXIT, Stream.EmitBlockInfoAbbrev(clang::serialized_diags::BLOCK_DIAG,
                                                       Abbrev));

  Stream.ExitBlock();
}

void SDiagsWriter::EmitMetaBlock() {
  llvm::BitstreamWriter &Stream = State->Stream;
  AbbreviationMap &Abbrevs = State->Abbrevs;

  Stream.EnterSubblock(clang::serialized_diags::BLOCK_META, 3);
  RecordData::value_type Record[] = {clang::serialized_diags::RECORD_VERSION, clang::serialized_diags::VersionNumber};
  Stream.EmitRecordWithAbbrev(Abbrevs.get(clang::serialized_diags::RECORD_VERSION), Record);
  Stream.ExitBlock();
}

unsigned SDiagsWriter::getEmitCategory(unsigned int category) {
  if (!State->Categories.insert(category).second)
    return category;

  // We use a local version of 'Record' so that we can be generating
  // another record when we lazily generate one for the category entry.
  StringRef catName = DiagnosticIDs::getCategoryNameFromID(category);
  RecordData::value_type Record[] = {clang::serialized_diags::RECORD_CATEGORY, category, catName.size()};
  State->Stream.EmitRecordWithBlob(State->Abbrevs.get(clang::serialized_diags::RECORD_CATEGORY), Record,
                                   catName);

  return category;
}

unsigned SDiagsWriter::getEmitDiagnosticFlag(DiagnosticsEngine::Level DiagLevel,
                                             unsigned DiagID) {
  if (DiagLevel == DiagnosticsEngine::Note)
    return 0; // No flag for notes.

  StringRef FlagName = DiagnosticIDs::getWarningOptionForDiag(DiagID);
  return getEmitDiagnosticFlag(FlagName);
}

unsigned SDiagsWriter::getEmitDiagnosticFlag(StringRef FlagName) {
  if (FlagName.empty())
    return 0;

  // Here we assume that FlagName points to static data whose pointer
  // value is fixed.  This allows us to unique by diagnostic groups.
  const void *data = FlagName.data();
  std::pair<unsigned, StringRef> &entry = State->DiagFlags[data];
  if (entry.first == 0) {
    entry.first = State->DiagFlags.size();
    entry.second = FlagName;

    // Lazily emit the string in a separate record.
    RecordData::value_type Record[] = {clang::serialized_diags::RECORD_DIAG_FLAG, entry.first,
                                       FlagName.size()};
    State->Stream.EmitRecordWithBlob(State->Abbrevs.get(clang::serialized_diags::RECORD_DIAG_FLAG),
                                     Record, FlagName);
  }

  return entry.first;
}

void SDiagsWriter::HandleDiagnostic(DiagnosticsEngine::Level DiagLevel,
                                    const Diagnostic &Info) {
  assert(!IsFinishing &&
         "Received a diagnostic after we've already started teardown.");
  if (IsFinishing) {
    SmallString<256> diagnostic;
    Info.FormatDiagnostic(diagnostic);
    // TODO(polac24): implement error handling
    return;
  }

  // Enter the block for a non-note diagnostic immediately, rather than waiting
  // for beginDiagnostic, in case associated notes are emitted before we get
  // there.
  if (DiagLevel != DiagnosticsEngine::Note) {
    if (State->EmittedAnyDiagBlocks)
      ExitDiagBlock();

    EnterDiagBlock();
    State->EmittedAnyDiagBlocks = true;
  }

  // Compute the diagnostic text.
  State->diagBuf.clear();
  Info.FormatDiagnostic(State->diagBuf);

  if (Info.getLocation().isInvalid()) {
    // Special-case diagnostics with no location. We may not have entered a
    // source file in this case, so we can't use the normal DiagnosticsRenderer
    // machinery.

    // Make sure we bracket all notes as "sub-diagnostics".  This matches
    // the behavior in SDiagsRenderer::emitDiagnostic().
    if (DiagLevel == DiagnosticsEngine::Note)
      EnterDiagBlock();

    EmitDiagnosticMessage(FullSourceLoc(), PresumedLoc(), DiagLevel,
                          State->diagBuf, &Info);

    if (DiagLevel == DiagnosticsEngine::Note)
      ExitDiagBlock();

    return;
  }

  assert(Info.hasSourceManager() && LangOpts &&
         "Unexpected diagnostic with valid location outside of a source file");
  SDiagsRenderer Renderer(*this, *LangOpts, &*State->DiagOpts);
  Renderer.emitDiagnostic(
      FullSourceLoc(Info.getLocation(), Info.getSourceManager()), DiagLevel,
      State->diagBuf, Info.getRanges(), Info.getFixItHints(), &Info);
}

static serialized_diags::Level getStableLevel(DiagnosticsEngine::Level Level) {
  switch (Level) {
#define CASE(X) case DiagnosticsEngine::X: return serialized_diags::X;
  CASE(Ignored)
  CASE(Note)
  CASE(Remark)
  CASE(Warning)
  CASE(Error)
  CASE(Fatal)
#undef CASE
  }

  llvm_unreachable("invalid diagnostic level");
}

void SDiagsWriter::EmitDiagnosticMessage(FullSourceLoc Loc, PresumedLoc PLoc,
                                         DiagnosticsEngine::Level Level,
                                         StringRef Message,
                                         DiagOrStoredDiag D) {
  llvm::BitstreamWriter &Stream = State->Stream;
  RecordData &Record = State->Record;
  AbbreviationMap &Abbrevs = State->Abbrevs;

  // Emit the RECORD_DIAG record.
  Record.clear();
    Record.push_back(clang::serialized_diags::RECORD_DIAG);
  Record.push_back(getStableLevel(Level));
  AddLocToRecord(Loc, PLoc, Record);

  if (const Diagnostic *Info = D.dyn_cast<const Diagnostic*>()) {
    // Emit the category string lazily and get the category ID.
    unsigned DiagID = DiagnosticIDs::getCategoryNumberForDiag(Info->getID());
    Record.push_back(getEmitCategory(DiagID));
    // Emit the diagnostic flag string lazily and get the mapped ID.
    Record.push_back(getEmitDiagnosticFlag(Level, Info->getID()));
  } else {
    Record.push_back(getEmitCategory());
    Record.push_back(getEmitDiagnosticFlag(Level));
  }

  Record.push_back(Message.size());
  Stream.EmitRecordWithBlob(Abbrevs.get(clang::serialized_diags::RECORD_DIAG), Record, Message);
}

void SDiagsRenderer::emitDiagnosticMessage(
    FullSourceLoc Loc, PresumedLoc PLoc, DiagnosticsEngine::Level Level,
    StringRef Message, ArrayRef<clang::CharSourceRange> Ranges,
    DiagOrStoredDiag D) {
  Writer.EmitDiagnosticMessage(Loc, PLoc, Level, Message, D);
}

void SDiagsWriter::EnterDiagBlock() {
  State->Stream.EnterSubblock(clang::serialized_diags::BLOCK_DIAG, 4);
}

void SDiagsWriter::ExitDiagBlock() {
  State->Stream.ExitBlock();
}

void SDiagsRenderer::beginDiagnostic(DiagOrStoredDiag D,
                                     DiagnosticsEngine::Level Level) {
  if (Level == DiagnosticsEngine::Note)
    Writer.EnterDiagBlock();
}

void SDiagsRenderer::endDiagnostic(DiagOrStoredDiag D,
                                   DiagnosticsEngine::Level Level) {
  // Only end note diagnostics here, because we can't be sure when we've seen
  // the last note associated with a non-note diagnostic.
  if (Level == DiagnosticsEngine::Note)
    Writer.ExitDiagBlock();
}

void SDiagsWriter::EmitCodeContext(SmallVectorImpl<CharSourceRange> &Ranges,
                                   ArrayRef<FixItHint> Hints,
                                   const SourceManager &SM) {
  llvm::BitstreamWriter &Stream = State->Stream;
  RecordData &Record = State->Record;
  AbbreviationMap &Abbrevs = State->Abbrevs;

  // Emit Source Ranges.
  for (ArrayRef<CharSourceRange>::iterator I = Ranges.begin(), E = Ranges.end();
       I != E; ++I)
    if (I->isValid())
      EmitCharSourceRange(*I, SM);

  // Emit FixIts.
  for (ArrayRef<FixItHint>::iterator I = Hints.begin(), E = Hints.end();
       I != E; ++I) {
    const FixItHint &Fix = *I;
    if (Fix.isNull())
      continue;
    Record.clear();
    Record.push_back(clang::serialized_diags::RECORD_FIXIT);
    AddCharSourceRangeToRecord(Fix.RemoveRange, Record, SM);
    Record.push_back(Fix.CodeToInsert.size());
    Stream.EmitRecordWithBlob(Abbrevs.get(clang::serialized_diags::RECORD_FIXIT), Record,
                              Fix.CodeToInsert);
  }
}

void SDiagsRenderer::emitCodeContext(FullSourceLoc Loc,
                                     DiagnosticsEngine::Level Level,
                                     SmallVectorImpl<CharSourceRange> &Ranges,
                                     ArrayRef<FixItHint> Hints) {
  Writer.EmitCodeContext(Ranges, Hints, Loc.getManager());
}

void SDiagsRenderer::emitNote(FullSourceLoc Loc, StringRef Message) {
  Writer.EnterDiagBlock();
  PresumedLoc PLoc = Loc.hasManager() ? Loc.getPresumedLoc() : PresumedLoc();
  Writer.EmitDiagnosticMessage(Loc, PLoc, DiagnosticsEngine::Note, Message,
                               DiagOrStoredDiag());
  Writer.ExitDiagBlock();
}

DiagnosticsEngine *SDiagsWriter::getMetaDiags() {
  // FIXME: It's slightly absurd to create a new diagnostics engine here, but
  // the other options that are available today are worse:
  //
  // 1. Teach DiagnosticsConsumers to emit diagnostics to the engine they are a
  //    part of. The DiagnosticsEngine would need to know not to send
  //    diagnostics back to the consumer that failed. This would require us to
  //    rework ChainedDiagnosticsConsumer and teach the engine about multiple
  //    consumers, which is difficult today because most APIs interface with
  //    consumers rather than the engine itself.
  //
  // 2. Pass a DiagnosticsEngine to SDiagsWriter on creation - this would need
  //    to be distinct from the engine the writer was being added to and would
  //    normally not be used.
  if (!State->MetaDiagnostics) {
    IntrusiveRefCntPtr<DiagnosticIDs> IDs(new DiagnosticIDs());
    auto Client =
        new TextDiagnosticPrinter(llvm::errs(), State->DiagOpts.get());
    State->MetaDiagnostics = std::make_unique<DiagnosticsEngine>(
        IDs, State->DiagOpts.get(), Client);
  }
  return State->MetaDiagnostics.get();
}

void SDiagsWriter::RemoveOldDiagnostics() {
  if (!llvm::sys::fs::remove(State->OutputFile))
    return;

  // TODO(polac24): implement error handling
  // Disable merging child records, as whatever is in this file may be
  // misleading.
  MergeChildRecords = false;
}

void SDiagsWriter::finish() {
  assert(!IsFinishing);
  IsFinishing = true;

  // The original instance is responsible for writing the file.
  if (!OriginalInstance)
    return;

  // Finish off any diagnostic we were in the process of emitting.
  if (State->EmittedAnyDiagBlocks)
    ExitDiagBlock();

  if (MergeChildRecords) {
    if (!State->EmittedAnyDiagBlocks)
      // We have no diagnostics of our own, so we can just leave the child
      // process' output alone
      return;

    if (llvm::sys::fs::exists(State->OutputFile))
        if (SDiagsMerger(*this).mergeRecordsFromFile(State->OutputFile.c_str())){
          // TODO(polac24): implement error handling
        }
  }

  std::error_code EC;
  auto OS = std::make_unique<llvm::raw_fd_ostream>(State->OutputFile.c_str(),
                                                    EC, llvm::sys::fs::OF_None);
  if (EC) {
    // TODO(polac24): implement error handling - write to the stderr
    OS->clear_error();
    return;
  }

  // Write the generated bitstream to "Out".
  OS->write((char *)&State->Buffer.front(), State->Buffer.size());
  OS->flush();

  assert(!OS->has_error());
  if (OS->has_error()) {
    // TODO(polac24): implement error handling - write to the stderr
    OS->clear_error();
  }
}



std::error_code SDiagsMerger::visitStartOfDiagnostic() {
  Writer.EnterDiagBlock();
  return std::error_code();
}

std::error_code SDiagsMerger::visitEndOfDiagnostic() {
  Writer.ExitDiagBlock();
  return std::error_code();
}

std::error_code
SDiagsMerger::visitSourceRangeRecord(const serialized_diags::Location &Start,
                                     const serialized_diags::Location &End) {
  RecordData::value_type Record[] = {
      clang::serialized_diags::RECORD_SOURCE_RANGE, FileLookup[Start.FileID], Start.Line, Start.Col,
      Start.Offset, FileLookup[End.FileID], End.Line, End.Col, End.Offset};
  Writer.State->Stream.EmitRecordWithAbbrev(
      Writer.State->Abbrevs.get(clang::serialized_diags::RECORD_SOURCE_RANGE), Record);
  return std::error_code();
}

std::error_code SDiagsMerger::visitDiagnosticRecord(
    unsigned Severity, const serialized_diags::Location &Location,
    unsigned Category, unsigned Flag, StringRef Message) {
  RecordData::value_type Record[] = {
      clang::serialized_diags::RECORD_DIAG, Severity, FileLookup[Location.FileID], Location.Line,
      Location.Col, Location.Offset, CategoryLookup[Category],
      Flag ? DiagFlagLookup[Flag] : 0, Message.size()};

  Writer.State->Stream.EmitRecordWithBlob(
      Writer.State->Abbrevs.get(clang::serialized_diags::RECORD_DIAG), Record, Message);
  return std::error_code();
}

std::error_code
SDiagsMerger::visitFixitRecord(const serialized_diags::Location &Start,
                               const serialized_diags::Location &End,
                               StringRef Text) {
  RecordData::value_type Record[] = {clang::serialized_diags::RECORD_FIXIT, FileLookup[Start.FileID],
                                     Start.Line, Start.Col, Start.Offset,
                                     FileLookup[End.FileID], End.Line, End.Col,
                                     End.Offset, Text.size()};

  Writer.State->Stream.EmitRecordWithBlob(
      Writer.State->Abbrevs.get(clang::serialized_diags::RECORD_FIXIT), Record, Text);
  return std::error_code();
}

std::error_code SDiagsMerger::visitFilenameRecord(unsigned ID, unsigned Size,
                                                  unsigned Timestamp,
                                                  StringRef Name) {
  FileLookup[ID] = Writer.getEmitFile(Name.str().c_str());
  return std::error_code();
}

std::error_code SDiagsMerger::visitCategoryRecord(unsigned ID, StringRef Name) {
  CategoryLookup[ID] = Writer.getEmitCategory(ID);
  return std::error_code();
}

std::error_code SDiagsMerger::visitDiagFlagRecord(unsigned ID, StringRef Name) {
  DiagFlagLookup[ID] = Writer.getEmitDiagnosticFlag(Name);
  return std::error_code();
}



void signalHandler( int signum ) {
  sharedWriter->finish();
  exit(signum);
}

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv);


  ExitOnError ExitOnErr("dia-merge: ");
  llvm::IntrusiveRefCntPtr<clang::DiagnosticOptions> DiagnosticOpts = new DiagnosticOptions();

    Remapper remapper;
    // Parse the the path remapping command line flags. This converts strings of
    // "X=Y" into a (regex, string) pair. Another way of looking at it: each
    // remap is equivalent to the s/pattern/replacement/ operator.
    auto errors = 0;
    for (const auto &remap : PathRemaps) {
      auto divider = remap.find('=');
      auto pattern = remap.substr(0, divider);
      try {
        std::regex re(pattern);
        auto replacement = remap.substr(divider + 1);
        remapper.addRemap(re, replacement);
      } catch (const std::regex_error &e) {
        errs() << "Error parsing regular expression: '" << pattern << "':\n"
               << e.what() << "\n";
        errors++;
      }
    }
            
    auto writer = SDiagsWriter(OutputFilename.c_str(), DiagnosticOpts.get(), false, remapper);
    auto merger = new SDiagsMerger(writer);

    if (Stream) {
        // save to a file once the process is interupted
        sharedWriter = &writer;
        signal(SIGINT, signalHandler);

        if (InputFilename.size() != 1) {
            errs() << "Error input: in the stream mode, provide only a single input file" << "\n";
            return -1;
        }
        auto filename = InputFilename[0];
        // loop merging the steram file (fifo) until an interrupt signal
        while(true) {
            merger->mergeRecordsFromFile(filename.c_str());
        }
    } else {
        for (const auto &filename : InputFilename) {
            merger->mergeRecordsFromFile(filename.c_str());
        }
    }
    writer.finish();
  return EXIT_SUCCESS;
}
