//////////////////////////////////////////////////////////////////////
//
//  RISESceneEditorBridge.h - Objective-C interface exposing the
//    library's SceneEditorSuggestions::SuggestionEngine and
//    SceneGrammar to Swift.  Pure Objective-C so it can be
//    imported via the Swift bridging header.
//
//    Two families of API on the single bridge object:
//      - allChunkKeywords — used by the syntax highlighter so the
//        126-keyword list is no longer a Swift literal.
//      - suggestions: — used by the scene editor's right-click
//        menu and (future) inline completion.
//
//////////////////////////////////////////////////////////////////////

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, RISESuggestionMode) {
    RISESuggestionModeContextMenu,
    RISESuggestionModeInlineCompletion
};

typedef NS_ENUM(NSInteger, RISESuggestionKind) {
    RISESuggestionKindChunkKeyword,
    RISESuggestionKindParameter,
    RISESuggestionKindEnumValue,
    RISESuggestionKindBoolLiteral,
    RISESuggestionKindReference
};

@interface RISESuggestion : NSObject
@property (readonly) NSString *insertText;
@property (readonly) NSString *displayText;
@property (readonly) NSString *descriptionText;
@property (readonly) RISESuggestionKind kind;
@property (readonly) NSString *categoryDisplayName;
@property (readonly) BOOL isUnambiguousCompletion;
@end

@interface RISESceneEditorBridge : NSObject

- (instancetype)init;

/// All chunk keywords the scene parser accepts.  Use from the syntax
/// highlighter instead of a hard-coded list.
- (NSSet<NSString *> *)allChunkKeywords;

/// Ranked suggestions for the given buffer and cursor position.
/// `cursorByte` is a UTF-8 byte offset into `text` — convert from
/// NSString character indices using `text.data(using: .utf8)` bytes
/// up to the character before returning.
- (NSArray<RISESuggestion *> *)suggestionsForText:(NSString *)text
                                       cursorByte:(NSUInteger)cursorByte
                                             mode:(RISESuggestionMode)mode;

@end

NS_ASSUME_NONNULL_END
