//////////////////////////////////////////////////////////////////////
//
//  RISESceneEditorBridge.mm - Objective-C++ bridge between Swift
//    and the C++ SceneEditorSuggestions module.  Converts strings
//    at the boundary and forwards calls to SceneGrammar /
//    SuggestionEngine.  Thread-safe for concurrent reads on the
//    static SceneGrammar singleton (initialized via C++11 magic
//    statics).
//
//////////////////////////////////////////////////////////////////////

#import "RISESceneEditorBridge.h"

#include <memory>
#include <string>

#include "SceneEditorSuggestions/SceneGrammar.h"
#include "SceneEditorSuggestions/SuggestionEngine.h"

using namespace RISE;
using namespace RISE::SceneEditorSuggestions;

@interface RISESuggestion ()
- (instancetype)initWithInsert:(NSString *)insertText
                       display:(NSString *)displayText
                   description:(NSString *)descriptionText
                          kind:(RISESuggestionKind)kind
               categoryDisplay:(NSString *)categoryDisplayName
                   unambiguous:(BOOL)unambiguous;
@end

@implementation RISESuggestion

- (instancetype)initWithInsert:(NSString *)insertText
                       display:(NSString *)displayText
                   description:(NSString *)descriptionText
                          kind:(RISESuggestionKind)kind
               categoryDisplay:(NSString *)categoryDisplayName
                   unambiguous:(BOOL)unambiguous
{
    self = [super init];
    if (self) {
        _insertText              = [insertText copy];
        _displayText             = [displayText copy];
        _descriptionText         = [descriptionText copy];
        _kind                    = kind;
        _categoryDisplayName     = [categoryDisplayName copy];
        _isUnambiguousCompletion = unambiguous;
    }
    return self;
}

@end

static RISESuggestionKind BridgeKind(SuggestionKind k)
{
    switch (k) {
        case SuggestionKind::ChunkKeyword: return RISESuggestionKindChunkKeyword;
        case SuggestionKind::Parameter:    return RISESuggestionKindParameter;
        case SuggestionKind::EnumValue:    return RISESuggestionKindEnumValue;
        case SuggestionKind::BoolLiteral:  return RISESuggestionKindBoolLiteral;
        case SuggestionKind::Reference:    return RISESuggestionKindReference;
    }
    return RISESuggestionKindChunkKeyword;
}

static SuggestionMode BridgeMode(RISESuggestionMode m)
{
    return m == RISESuggestionModeInlineCompletion
        ? SuggestionMode::InlineCompletion
        : SuggestionMode::ContextMenu;
}

@implementation RISESceneEditorBridge {
    std::unique_ptr<SuggestionEngine> _engine;
}

- (instancetype)init
{
    self = [super init];
    if (self) {
        _engine = std::make_unique<SuggestionEngine>();
    }
    return self;
}

- (NSSet<NSString *> *)allChunkKeywords
{
    const SceneGrammar& grammar = SceneGrammar::Instance();
    const std::vector<std::string>& kws = grammar.AllChunkKeywords();
    NSMutableSet<NSString *>* set = [NSMutableSet setWithCapacity:(NSUInteger)kws.size()];
    for (const std::string& kw : kws) {
        [set addObject:[NSString stringWithUTF8String:kw.c_str()]];
    }
    return set;
}

- (NSArray<RISESuggestion *> *)suggestionsForText:(NSString *)text
                                       cursorByte:(NSUInteger)cursorByte
                                             mode:(RISESuggestionMode)mode
{
    const char* utf8 = [text UTF8String];
    const std::string buffer = utf8 ? std::string(utf8) : std::string();
    const std::vector<Suggestion> result = _engine->GetSuggestions(
        buffer,
        static_cast<std::size_t>(cursorByte),
        BridgeMode(mode));

    NSMutableArray<RISESuggestion *>* out = [NSMutableArray arrayWithCapacity:(NSUInteger)result.size()];
    const SceneGrammar& grammar = SceneGrammar::Instance();
    for (const Suggestion& s : result) {
        RISESuggestion* bridged = [[RISESuggestion alloc]
            initWithInsert:[NSString stringWithUTF8String:s.insertText.c_str()]
                   display:[NSString stringWithUTF8String:s.displayText.c_str()]
               description:[NSString stringWithUTF8String:s.description.c_str()]
                      kind:BridgeKind(s.kind)
           categoryDisplay:[NSString stringWithUTF8String:grammar.CategoryDisplayName(s.category)]
               unambiguous:s.isUnambiguousCompletion ? YES : NO];
        [out addObject:bridged];
    }
    return out;
}

@end
