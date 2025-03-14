/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99: */

// Copyright 2012 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "irregexp/RegExpEngine.h"

#include "irregexp/NativeRegExpMacroAssembler.h"
#include "irregexp/RegExpCharacters.h"
#include "irregexp/RegExpMacroAssembler.h"
#include "jit/ExecutableAllocator.h"
#include "jit/JitCommon.h"

#include "irregexp/RegExpCharacters-inl.h"

using namespace js;
using namespace js::irregexp;

using mozilla::ArrayLength;
using mozilla::DebugOnly;
using mozilla::Maybe;

#define DEFINE_ACCEPT(Type)                                          \
    void Type##Node::Accept(NodeVisitor* visitor) {                  \
        visitor->Visit##Type(this);                                  \
    }
FOR_EACH_NODE_TYPE(DEFINE_ACCEPT)
#undef DEFINE_ACCEPT

void LoopChoiceNode::Accept(NodeVisitor* visitor) {
    visitor->VisitLoopChoice(this);
}

static const int kMaxLookaheadForBoyerMoore = 8;

RegExpNode::RegExpNode(LifoAlloc* alloc)
  : replacement_(nullptr), trace_count_(0), alloc_(alloc)
{
    bm_info_[0] = bm_info_[1] = nullptr;
}

static const int kMaxOneByteCharCode = 0xff;
static const int kMaxUtf16CodeUnit = 0xffff;

static char16_t
MaximumCharacter(bool ascii)
{
    return ascii ? kMaxOneByteCharCode : kMaxUtf16CodeUnit;
}

static void
AddClass(const int* elmv, int elmc,
         CharacterRangeVector* ranges)
{
    elmc--;
    MOZ_ASSERT(elmv[elmc] == 0x10000);
    for (int i = 0; i < elmc; i += 2) {
        MOZ_ASSERT(elmv[i] < elmv[i + 1]);
        ranges->append(CharacterRange(elmv[i], elmv[i + 1] - 1));
    }
}

static void
AddClassNegated(const int* elmv,
                int elmc,
                CharacterRangeVector* ranges)
{
    elmc--;
    MOZ_ASSERT(elmv[elmc] == 0x10000);
    MOZ_ASSERT(elmv[0] != 0x0000);
    MOZ_ASSERT(elmv[elmc-1] != kMaxUtf16CodeUnit);
    char16_t last = 0x0000;
    for (int i = 0; i < elmc; i += 2) {
        MOZ_ASSERT(last <= elmv[i] - 1);
        MOZ_ASSERT(elmv[i] < elmv[i + 1]);
        ranges->append(CharacterRange(last, elmv[i] - 1));
        last = elmv[i + 1];
    }
    ranges->append(CharacterRange(last, kMaxUtf16CodeUnit));
}

void
CharacterRange::AddClassEscape(LifoAlloc* alloc, char16_t type,
			       CharacterRangeVector* ranges)
{
    switch (type) {
      case 's':
        AddClass(kSpaceRanges, kSpaceRangeCount, ranges);
        break;
      case 'S':
        AddClassNegated(kSpaceRanges, kSpaceRangeCount, ranges);
        break;
      case 'w':
        AddClass(kWordRanges, kWordRangeCount, ranges);
        break;
      case 'W':
        AddClassNegated(kWordRanges, kWordRangeCount, ranges);
        break;
      case 'd':
        AddClass(kDigitRanges, kDigitRangeCount, ranges);
        break;
      case 'D':
        AddClassNegated(kDigitRanges, kDigitRangeCount, ranges);
        break;
      case '.':
        AddClassNegated(kLineTerminatorRanges, kLineTerminatorRangeCount, ranges);
        break;
        // This is not a character range as defined by the spec but a
        // convenient shorthand for a character class that matches any
        // character.
      case '*':
        ranges->append(CharacterRange::Everything());
        break;
        // This is the set of characters matched by the $ and ^ symbols
        // in multiline mode.
      case 'n':
        AddClass(kLineTerminatorRanges, kLineTerminatorRangeCount, ranges);
        break;
      default:
        MOZ_CRASH("Bad character class escape");
    }
}

// Add class escape, excluding surrogate pair range.
void
CharacterRange::AddClassEscapeUnicode(LifoAlloc* alloc, char16_t type,
                                      CharacterRangeVector* ranges, bool ignore_case)
{
    switch (type) {
      case 's':
      case 'd':
        return AddClassEscape(alloc, type, ranges);
        break;
      case 'S':
        AddClassNegated(kSpaceAndSurrogateRanges, kSpaceAndSurrogateRangeCount, ranges);
        break;
      case 'w':
        if (ignore_case)
            AddClass(kIgnoreCaseWordRanges, kIgnoreCaseWordRangeCount, ranges);
        else
            AddClassEscape(alloc, type, ranges);
        break;
      case 'W':
        if (ignore_case) {
            AddClass(kNegatedIgnoreCaseWordAndSurrogateRanges,
                     kNegatedIgnoreCaseWordAndSurrogateRangeCount, ranges);
        } else {
            AddClassNegated(kWordAndSurrogateRanges, kWordAndSurrogateRangeCount, ranges);
        }
        break;
      case 'D':
        AddClassNegated(kDigitAndSurrogateRanges, kDigitAndSurrogateRangeCount, ranges);
        break;
      default:
        MOZ_CRASH("Bad type!");
    }
}

static bool
RangesContainLatin1Equivalents(const CharacterRangeVector& ranges, bool unicode)
{
    for (size_t i = 0; i < ranges.length(); i++) {
        // TODO(dcarney): this could be a lot more efficient.
        if (RangeContainsLatin1Equivalents(ranges[i], unicode))
            return true;
    }
    return false;
}

static const size_t kEcma262UnCanonicalizeMaxWidth = 4;

// Returns the number of characters in the equivalence class, omitting those
// that cannot occur in the source string if it is a one byte string.
static int
GetCaseIndependentLetters(char16_t character,
                          bool ascii_subject,
                          bool unicode,
                          const char16_t* choices,
                          size_t choices_length,
                          char16_t* letters)
{
    size_t count = 0;
    for (size_t i = 0; i < choices_length; i++) {
        char16_t c = choices[i];

        // Skip characters that can't appear in one byte strings.
        if (!unicode && ascii_subject && c > kMaxOneByteCharCode)
            continue;

        // Watch for duplicates.
        bool found = false;
        for (size_t j = 0; j < count; j++) {
            if (letters[j] == c) {
                found = true;
                break;
            }
        }
        if (found)
            continue;

        letters[count++] = c;
    }

    return count;
}

static int
GetCaseIndependentLetters(char16_t character,
                          bool ascii_subject,
                          bool unicode,
                          char16_t* letters)
{
    if (unicode) {
        const char16_t choices[] = {
            character,
            unicode::FoldCase(character),
            unicode::ReverseFoldCase1(character),
            unicode::ReverseFoldCase2(character),
            unicode::ReverseFoldCase3(character),
        };
        return GetCaseIndependentLetters(character, ascii_subject, unicode,
                                         choices, ArrayLength(choices), letters);
    }

    char16_t upper = unicode::ToUpperCase(character);
    unicode::CodepointsWithSameUpperCase others(character);
    char16_t other1 = others.other1();
    char16_t other2 = others.other2();
    char16_t other3 = others.other3();

    // ES 2017 draft 996af87b7072b3c3dd2b1def856c66f456102215 21.2.4.2
    // step 3.g.
    // The standard requires that non-ASCII characters cannot have ASCII
    // character codes in their equivalence class, even though this
    // situation occurs multiple times in the Unicode tables.
    static const unsigned kMaxAsciiCharCode = 127;
    if (upper <= kMaxAsciiCharCode) {
        if (character > kMaxAsciiCharCode) {
            // If Canonicalize(character) == character, all other characters
            // should be ignored.
            return GetCaseIndependentLetters(character, ascii_subject, unicode,
                                             &character, 1, letters);
        }

        if (other1 > kMaxAsciiCharCode)
            other1 = character;
        if (other2 > kMaxAsciiCharCode)
            other2 = character;
        if (other3 > kMaxAsciiCharCode)
            other3 = character;
    }

    const char16_t choices[] = {
        character,
        upper,
        other1,
        other2,
        other3
    };
    return GetCaseIndependentLetters(character, ascii_subject, unicode,
                                     choices, ArrayLength(choices), letters);
}

void
CharacterRange::AddCaseEquivalents(bool is_ascii, bool unicode, CharacterRangeVector* ranges)
{
    char16_t bottom = from();
    char16_t top = to();

    if (is_ascii && !RangeContainsLatin1Equivalents(*this, unicode)) {
        if (bottom > kMaxOneByteCharCode)
            return;
        if (top > kMaxOneByteCharCode)
            top = kMaxOneByteCharCode;
    }

    for (char16_t c = bottom;; c++) {
        char16_t chars[kEcma262UnCanonicalizeMaxWidth];
        size_t length = GetCaseIndependentLetters(c, is_ascii, unicode, chars);

        for (size_t i = 0; i < length; i++) {
            char16_t other = chars[i];
            if (other == c)
                continue;

            // Try to combine with an existing range.
            bool found = false;
            for (size_t i = 0; i < ranges->length(); i++) {
                CharacterRange& range = (*ranges)[i];
                if (range.Contains(other)) {
                    found = true;
                    break;
                } else if (other == range.from() - 1) {
                    range.set_from(other);
                    found = true;
                    break;
                } else if (other == range.to() + 1) {
                    range.set_to(other);
                    found = true;
                    break;
                }
            }

            if (!found)
                ranges->append(CharacterRange::Singleton(other));
        }

        if (c == top)
            break;
    }
}

static bool
CompareInverseRanges(const CharacterRangeVector& ranges, const int* special_class, size_t length)
{
    length--;  // Remove final 0x10000.
    MOZ_ASSERT(special_class[length] == 0x10000);
    MOZ_ASSERT(ranges.length() != 0);
    MOZ_ASSERT(length != 0);
    MOZ_ASSERT(special_class[0] != 0);
    if (ranges.length() != (length >> 1) + 1)
        return false;
    CharacterRange range = ranges[0];
    if (range.from() != 0)
        return false;
    for (size_t i = 0; i < length; i += 2) {
        if (special_class[i] != (range.to() + 1))
            return false;
        range = ranges[(i >> 1) + 1];
        if (special_class[i+1] != range.from())
            return false;
    }
    if (range.to() != 0xffff)
        return false;
    return true;
}

static bool
CompareRanges(const CharacterRangeVector& ranges, const int* special_class, size_t length)
{
    length--;  // Remove final 0x10000.
    MOZ_ASSERT(special_class[length] == 0x10000);
    if (ranges.length() * 2 != length)
        return false;
    for (size_t i = 0; i < length; i += 2) {
        CharacterRange range = ranges[i >> 1];
        if (range.from() != special_class[i] || range.to() != special_class[i + 1] - 1)
            return false;
    }
    return true;
}

bool
RegExpCharacterClass::is_standard(LifoAlloc* alloc)
{
    // TODO(lrn): Remove need for this function, by not throwing away information
    // along the way.
    if (is_negated_)
        return false;
    if (set_.is_standard())
        return true;
    if (CompareRanges(set_.ranges(alloc), kSpaceRanges, kSpaceRangeCount)) {
        set_.set_standard_set_type('s');
        return true;
    }
    if (CompareInverseRanges(set_.ranges(alloc), kSpaceRanges, kSpaceRangeCount)) {
        set_.set_standard_set_type('S');
        return true;
    }
    if (CompareInverseRanges(set_.ranges(alloc),
                             kLineTerminatorRanges,
                             kLineTerminatorRangeCount)) {
        set_.set_standard_set_type('.');
        return true;
    }
    if (CompareRanges(set_.ranges(alloc),
                      kLineTerminatorRanges,
                      kLineTerminatorRangeCount)) {
        set_.set_standard_set_type('n');
        return true;
    }
    if (CompareRanges(set_.ranges(alloc), kWordRanges, kWordRangeCount)) {
        set_.set_standard_set_type('w');
        return true;
    }
    if (CompareInverseRanges(set_.ranges(alloc), kWordRanges, kWordRangeCount)) {
        set_.set_standard_set_type('W');
        return true;
    }
    return false;
}

bool
CharacterRange::IsCanonical(const CharacterRangeVector& ranges)
{
    int n = ranges.length();
    if (n <= 1)
        return true;

    int max = ranges[0].to();
    for (int i = 1; i < n; i++) {
        CharacterRange next_range = ranges[i];
        if (next_range.from() <= max + 1)
            return false;
        max = next_range.to();
    }
    return true;
}

// Move a number of elements in a zonelist to another position
// in the same list. Handles overlapping source and target areas.
static
void MoveRanges(CharacterRangeVector& list, int from, int to, int count)
{
    // Ranges are potentially overlapping.
    if (from < to) {
        for (int i = count - 1; i >= 0; i--)
            list[to + i] = list[from + i];
    } else {
        for (int i = 0; i < count; i++)
            list[to + i] = list[from + i];
    }
}

static int
InsertRangeInCanonicalList(CharacterRangeVector& list,
                           int count,
                           CharacterRange insert)
{
    // Inserts a range into list[0..count[, which must be sorted
    // by from value and non-overlapping and non-adjacent, using at most
    // list[0..count] for the result. Returns the number of resulting
    // canonicalized ranges. Inserting a range may collapse existing ranges into
    // fewer ranges, so the return value can be anything in the range 1..count+1.
    char16_t from = insert.from();
    char16_t to = insert.to();
    int start_pos = 0;
    int end_pos = count;
    for (int i = count - 1; i >= 0; i--) {
        CharacterRange current = list[i];
        if (current.from() > to + 1) {
            end_pos = i;
        } else if (current.to() + 1 < from) {
            start_pos = i + 1;
            break;
        }
    }

    // Inserted range overlaps, or is adjacent to, ranges at positions
    // [start_pos..end_pos[. Ranges before start_pos or at or after end_pos are
    // not affected by the insertion.
    // If start_pos == end_pos, the range must be inserted before start_pos.
    // if start_pos < end_pos, the entire range from start_pos to end_pos
    // must be merged with the insert range.

    if (start_pos == end_pos) {
        // Insert between existing ranges at position start_pos.
        if (start_pos < count) {
            MoveRanges(list, start_pos, start_pos + 1, count - start_pos);
        }
        list[start_pos] = insert;
        return count + 1;
    }
    if (start_pos + 1 == end_pos) {
        // Replace single existing range at position start_pos.
        CharacterRange to_replace = list[start_pos];
        int new_from = Min(to_replace.from(), from);
        int new_to = Max(to_replace.to(), to);
        list[start_pos] = CharacterRange(new_from, new_to);
        return count;
    }
    // Replace a number of existing ranges from start_pos to end_pos - 1.
    // Move the remaining ranges down.

    int new_from = Min(list[start_pos].from(), from);
    int new_to = Max(list[end_pos - 1].to(), to);
    if (end_pos < count) {
        MoveRanges(list, end_pos, start_pos + 1, count - end_pos);
    }
    list[start_pos] = CharacterRange(new_from, new_to);
    return count - (end_pos - start_pos) + 1;
}

void
CharacterRange::Canonicalize(CharacterRangeVector& character_ranges)
{
    if (character_ranges.length() <= 1) return;
    // Check whether ranges are already canonical (increasing, non-overlapping,
    // non-adjacent).
    int n = character_ranges.length();
    int max = character_ranges[0].to();
    int i = 1;
    while (i < n) {
        CharacterRange current = character_ranges[i];
        if (current.from() <= max + 1) {
            break;
        }
        max = current.to();
        i++;
    }
    // Canonical until the i'th range. If that's all of them, we are done.
    if (i == n) return;

    // The ranges at index i and forward are not canonicalized. Make them so by
    // doing the equivalent of insertion sort (inserting each into the previous
    // list, in order).
    // Notice that inserting a range can reduce the number of ranges in the
    // result due to combining of adjacent and overlapping ranges.
    int read = i;  // Range to insert.
    size_t num_canonical = i;  // Length of canonicalized part of list.
    do {
        num_canonical = InsertRangeInCanonicalList(character_ranges,
                                                   num_canonical,
                                                   character_ranges[read]);
        read++;
    } while (read < n);

    while (character_ranges.length() > num_canonical)
        character_ranges.popBack();

    MOZ_ASSERT(CharacterRange::IsCanonical(character_ranges));
}

// -------------------------------------------------------------------
// SeqRegExpNode

class VisitMarker
{
  public:
    explicit VisitMarker(NodeInfo* info)
      : info_(info)
    {
        MOZ_ASSERT(!info->visited);
        info->visited = true;
    }
    ~VisitMarker() {
        info_->visited = false;
    }
  private:
    NodeInfo* info_;
};

bool
SeqRegExpNode::FillInBMInfo(int offset,
                            int budget,
                            BoyerMooreLookahead* bm,
                            bool not_at_start)
{
    if (!bm->CheckOverRecursed())
        return false;
    if (!on_success_->FillInBMInfo(offset, budget - 1, bm, not_at_start))
        return false;
    if (offset == 0)
        set_bm_info(not_at_start, bm);
    return true;
}

RegExpNode*
SeqRegExpNode::FilterASCII(int depth, bool ignore_case, bool unicode)
{
    if (info()->replacement_calculated)
        return replacement();

    if (depth < 0)
        return this;

    MOZ_ASSERT(!info()->visited);
    VisitMarker marker(info());
    return FilterSuccessor(depth - 1, ignore_case, unicode);
}

RegExpNode*
SeqRegExpNode::FilterSuccessor(int depth, bool ignore_case, bool unicode)
{
    RegExpNode* next = on_success_->FilterASCII(depth - 1, ignore_case, unicode);
    if (next == nullptr)
        return set_replacement(nullptr);

    on_success_ = next;
    return set_replacement(this);
}

// -------------------------------------------------------------------
// ActionNode

int
ActionNode::EatsAtLeast(int still_to_find, int budget, bool not_at_start)
{
    if (budget <= 0)
        return 0;
    if (action_type_ == POSITIVE_SUBMATCH_SUCCESS)
        return 0;  // Rewinds input!
    return on_success()->EatsAtLeast(still_to_find,
                                     budget - 1,
                                     not_at_start);
}

bool
ActionNode::FillInBMInfo(int offset,
                         int budget,
                         BoyerMooreLookahead* bm,
                         bool not_at_start)
{
    if (!bm->CheckOverRecursed())
        return false;

    if (action_type_ == BEGIN_SUBMATCH) {
        bm->SetRest(offset);
    } else if (action_type_ != POSITIVE_SUBMATCH_SUCCESS) {
        if (!on_success()->FillInBMInfo(offset, budget - 1, bm, not_at_start))
            return false;
    }
    SaveBMInfo(bm, not_at_start, offset);

    return true;
}

/* static */ ActionNode*
ActionNode::SetRegister(int reg,
                        int val,
                        RegExpNode* on_success)
{
    ActionNode* result = on_success->alloc()->newInfallible<ActionNode>(SET_REGISTER, on_success);
    result->data_.u_store_register.reg = reg;
    result->data_.u_store_register.value = val;
    return result;
}

/* static */ ActionNode*
ActionNode::IncrementRegister(int reg, RegExpNode* on_success)
{
    ActionNode* result = on_success->alloc()->newInfallible<ActionNode>(INCREMENT_REGISTER, on_success);
    result->data_.u_increment_register.reg = reg;
    return result;
}

/* static */ ActionNode*
ActionNode::StorePosition(int reg, bool is_capture, RegExpNode* on_success)
{
    ActionNode* result = on_success->alloc()->newInfallible<ActionNode>(STORE_POSITION, on_success);
    result->data_.u_position_register.reg = reg;
    result->data_.u_position_register.is_capture = is_capture;
    return result;
}

/* static */ ActionNode*
ActionNode::ClearCaptures(Interval range, RegExpNode* on_success)
{
    ActionNode* result = on_success->alloc()->newInfallible<ActionNode>(CLEAR_CAPTURES, on_success);
    result->data_.u_clear_captures.range_from = range.from();
    result->data_.u_clear_captures.range_to = range.to();
    return result;
}

/* static */ ActionNode*
ActionNode::BeginSubmatch(int stack_pointer_reg, int position_reg, RegExpNode* on_success)
{
    ActionNode* result = on_success->alloc()->newInfallible<ActionNode>(BEGIN_SUBMATCH, on_success);
    result->data_.u_submatch.stack_pointer_register = stack_pointer_reg;
    result->data_.u_submatch.current_position_register = position_reg;
    return result;
}

/* static */ ActionNode*
ActionNode::PositiveSubmatchSuccess(int stack_pointer_reg,
                                    int restore_reg,
                                    int clear_capture_count,
                                    int clear_capture_from,
                                    RegExpNode* on_success)
{
    ActionNode* result = on_success->alloc()->newInfallible<ActionNode>(POSITIVE_SUBMATCH_SUCCESS, on_success);
    result->data_.u_submatch.stack_pointer_register = stack_pointer_reg;
    result->data_.u_submatch.current_position_register = restore_reg;
    result->data_.u_submatch.clear_register_count = clear_capture_count;
    result->data_.u_submatch.clear_register_from = clear_capture_from;
    return result;
}

/* static */ ActionNode*
ActionNode::EmptyMatchCheck(int start_register,
                            int repetition_register,
                            int repetition_limit,
                            RegExpNode* on_success)
{
    ActionNode* result = on_success->alloc()->newInfallible<ActionNode>(EMPTY_MATCH_CHECK, on_success);
    result->data_.u_empty_match_check.start_register = start_register;
    result->data_.u_empty_match_check.repetition_register = repetition_register;
    result->data_.u_empty_match_check.repetition_limit = repetition_limit;
    return result;
}

// -------------------------------------------------------------------
// TextNode

int
TextNode::EatsAtLeast(int still_to_find, int budget, bool not_at_start)
{
    int answer = Length();
    if (answer >= still_to_find)
        return answer;
    if (budget <= 0)
        return answer;

    // We are not at start after this node so we set the last argument to 'true'.
    return answer + on_success()->EatsAtLeast(still_to_find - answer,
                                              budget - 1,
                                              true);
}

int
TextNode::GreedyLoopTextLength()
{
    TextElement elm = elements()[elements().length() - 1];
    return elm.cp_offset() + elm.length();
}

RegExpNode*
TextNode::FilterASCII(int depth, bool ignore_case, bool unicode)
{
    if (info()->replacement_calculated)
        return replacement();

    if (depth < 0)
        return this;

    MOZ_ASSERT(!info()->visited);
    VisitMarker marker(info());
    int element_count = elements().length();
    for (int i = 0; i < element_count; i++) {
        TextElement elm = elements()[i];
        if (elm.text_type() == TextElement::ATOM) {
            CharacterVector& quarks = const_cast<CharacterVector&>(elm.atom()->data());
            for (size_t j = 0; j < quarks.length(); j++) {
                uint16_t c = quarks[j];
                if (c <= kMaxOneByteCharCode)
                    continue;
                if (!ignore_case)
                    return set_replacement(nullptr);

                // Here, we need to check for characters whose upper and lower cases
                // are outside the Latin-1 range.
                char16_t converted = ConvertNonLatin1ToLatin1(c, unicode);
                if (converted == 0) {
                    // Character is outside Latin-1 completely
                    return set_replacement(nullptr);
                }

                // Convert quark to Latin-1 in place.
                quarks[j] = converted;
            }
        } else {
            MOZ_ASSERT(elm.text_type() == TextElement::CHAR_CLASS);
            RegExpCharacterClass* cc = elm.char_class();

            CharacterRangeVector& ranges = cc->ranges(alloc());
            if (!CharacterRange::IsCanonical(ranges))
                CharacterRange::Canonicalize(ranges);

            // Now they are in order so we only need to look at the first.
            int range_count = ranges.length();
            if (cc->is_negated()) {
                if (range_count != 0 &&
                    ranges[0].from() == 0 &&
                    ranges[0].to() >= kMaxOneByteCharCode)
                {
                    // This will be handled in a later filter.
                    if (ignore_case && RangesContainLatin1Equivalents(ranges, unicode))
                        continue;
                    return set_replacement(nullptr);
                }
            } else {
                if (range_count == 0 ||
                    ranges[0].from() > kMaxOneByteCharCode)
                {
                    // This will be handled in a later filter.
                    if (ignore_case && RangesContainLatin1Equivalents(ranges, unicode))
                        continue;
                    return set_replacement(nullptr);
                }
            }
        }
    }
    return FilterSuccessor(depth - 1, ignore_case, unicode);
}

void
TextNode::CalculateOffsets()
{
    int element_count = elements().length();

    // Set up the offsets of the elements relative to the start.  This is a fixed
    // quantity since a TextNode can only contain fixed-width things.
    int cp_offset = 0;
    for (int i = 0; i < element_count; i++) {
        TextElement& elm = elements()[i];
        elm.set_cp_offset(cp_offset);
        cp_offset += elm.length();
    }
}

void TextNode::MakeCaseIndependent(bool is_ascii, bool unicode)
{
    int element_count = elements().length();
    for (int i = 0; i < element_count; i++) {
        TextElement elm = elements()[i];
        if (elm.text_type() == TextElement::CHAR_CLASS) {
            RegExpCharacterClass* cc = elm.char_class();

            // None of the standard character classes is different in the case
            // independent case and it slows us down if we don't know that.
            if (cc->is_standard(alloc()))
                continue;

            CharacterRangeVector& ranges = cc->ranges(alloc());
            int range_count = ranges.length();
            for (int j = 0; j < range_count; j++)
                ranges[j].AddCaseEquivalents(is_ascii, unicode, &ranges);
        }
    }
}

// -------------------------------------------------------------------
// AssertionNode

int
AssertionNode::EatsAtLeast(int still_to_find, int budget, bool not_at_start)
{
    if (budget <= 0)
        return 0;

    // If we know we are not at the start and we are asked "how many characters
    // will you match if you succeed?" then we can answer anything since false
    // implies false.  So lets just return the max answer (still_to_find) since
    // that won't prevent us from preloading a lot of characters for the other
    // branches in the node graph.
    if (assertion_type() == AT_START && not_at_start)
        return still_to_find;

    return on_success()->EatsAtLeast(still_to_find, budget - 1, not_at_start);
}

bool
AssertionNode::FillInBMInfo(int offset, int budget, BoyerMooreLookahead* bm, bool not_at_start)
{
    if (!bm->CheckOverRecursed())
        return false;

    // Match the behaviour of EatsAtLeast on this node.
    if (assertion_type() == AT_START && not_at_start)
        return true;

    if (!on_success()->FillInBMInfo(offset, budget - 1, bm, not_at_start))
        return false;
    SaveBMInfo(bm, not_at_start, offset);
    return true;
}

// -------------------------------------------------------------------
// BackReferenceNode

int
BackReferenceNode::EatsAtLeast(int still_to_find, int budget, bool not_at_start)
{
    if (budget <= 0)
        return 0;
    return on_success()->EatsAtLeast(still_to_find, budget - 1, not_at_start);
}

bool
BackReferenceNode::FillInBMInfo(int offset, int budget, BoyerMooreLookahead* bm, bool not_at_start)
{
    // Working out the set of characters that a backreference can match is too
    // hard, so we just say that any character can match.
    bm->SetRest(offset);
    SaveBMInfo(bm, not_at_start, offset);
    return true;
}

// -------------------------------------------------------------------
// ChoiceNode

int
ChoiceNode::EatsAtLeastHelper(int still_to_find,
                              int budget,
                              RegExpNode* ignore_this_node,
                              bool not_at_start)
{
    if (budget <= 0)
        return 0;

    int min = 100;
    size_t choice_count = alternatives().length();
    budget = (budget - 1) / choice_count;
    for (size_t i = 0; i < choice_count; i++) {
        RegExpNode* node = alternatives()[i].node();
        if (node == ignore_this_node) continue;
        int node_eats_at_least =
            node->EatsAtLeast(still_to_find, budget, not_at_start);
        if (node_eats_at_least < min)
            min = node_eats_at_least;
        if (min == 0)
            return 0;
    }
    return min;
}

int
ChoiceNode::EatsAtLeast(int still_to_find, int budget, bool not_at_start)
{
    return EatsAtLeastHelper(still_to_find,
                             budget,
                             nullptr,
                             not_at_start);
}

void
ChoiceNode::GetQuickCheckDetails(QuickCheckDetails* details,
                                 RegExpCompiler* compiler,
                                 int characters_filled_in,
                                 bool not_at_start)
{
    not_at_start = (not_at_start || not_at_start_);
    int choice_count = alternatives().length();
    MOZ_ASSERT(choice_count > 0);
    alternatives()[0].node()->GetQuickCheckDetails(details,
                                                   compiler,
                                                   characters_filled_in,
                                                   not_at_start);
    for (int i = 1; i < choice_count; i++) {
        QuickCheckDetails new_details(details->characters());
        RegExpNode* node = alternatives()[i].node();
        node->GetQuickCheckDetails(&new_details, compiler,
                                   characters_filled_in,
                                   not_at_start);
        // Here we merge the quick match details of the two branches.
        details->Merge(&new_details, characters_filled_in);
    }
}

bool
ChoiceNode::FillInBMInfo(int offset,
                         int budget,
                         BoyerMooreLookahead* bm,
                         bool not_at_start)
{
    if (!bm->CheckOverRecursed())
        return false;

    const GuardedAlternativeVector& alts = alternatives();
    budget = (budget - 1) / alts.length();
    for (size_t i = 0; i < alts.length(); i++) {
        const GuardedAlternative& alt = alts[i];
        if (alt.guards() != nullptr && alt.guards()->length() != 0) {
            bm->SetRest(offset);  // Give up trying to fill in info.
            SaveBMInfo(bm, not_at_start, offset);
            return true;
        }
        if (!alt.node()->FillInBMInfo(offset, budget, bm, not_at_start))
            return false;
    }
    SaveBMInfo(bm, not_at_start, offset);
    return true;
}

RegExpNode*
ChoiceNode::FilterASCII(int depth, bool ignore_case, bool unicode)
{
    if (info()->replacement_calculated)
        return replacement();
    if (depth < 0)
        return this;
    if (info()->visited)
        return this;
    VisitMarker marker(info());
    int choice_count = alternatives().length();

    for (int i = 0; i < choice_count; i++) {
        const GuardedAlternative alternative = alternatives()[i];
        if (alternative.guards() != nullptr && alternative.guards()->length() != 0) {
            set_replacement(this);
            return this;
        }
    }

    int surviving = 0;
    RegExpNode* survivor = nullptr;
    for (int i = 0; i < choice_count; i++) {
        GuardedAlternative alternative = alternatives()[i];
        RegExpNode* replacement =
            alternative.node()->FilterASCII(depth - 1, ignore_case, unicode);
        MOZ_ASSERT(replacement != this);  // No missing EMPTY_MATCH_CHECK.
        if (replacement != nullptr) {
            alternatives()[i].set_node(replacement);
            surviving++;
            survivor = replacement;
        }
    }
    if (surviving < 2)
        return set_replacement(survivor);

    set_replacement(this);
    if (surviving == choice_count)
        return this;

    // Only some of the nodes survived the filtering.  We need to rebuild the
    // alternatives list.
    GuardedAlternativeVector new_alternatives(*alloc());
    new_alternatives.reserve(surviving);
    for (int i = 0; i < choice_count; i++) {
        RegExpNode* replacement =
            alternatives()[i].node()->FilterASCII(depth - 1, ignore_case, unicode);
        if (replacement != nullptr) {
            alternatives()[i].set_node(replacement);
            new_alternatives.append(alternatives()[i]);
        }
    }

    alternatives_ = Move(new_alternatives);
    return this;
}

// -------------------------------------------------------------------
// NegativeLookaheadChoiceNode

bool
NegativeLookaheadChoiceNode::FillInBMInfo(int offset,
                                          int budget,
                                          BoyerMooreLookahead* bm,
                                          bool not_at_start)
{
    if (!bm->CheckOverRecursed())
        return false;

    if (!alternatives()[1].node()->FillInBMInfo(offset, budget - 1, bm, not_at_start))
        return false;
    if (offset == 0)
        set_bm_info(not_at_start, bm);
    return true;
}

int
NegativeLookaheadChoiceNode::EatsAtLeast(int still_to_find, int budget, bool not_at_start)
{
    if (budget <= 0)
        return 0;

    // Alternative 0 is the negative lookahead, alternative 1 is what comes
    // afterwards.
    RegExpNode* node = alternatives()[1].node();
    return node->EatsAtLeast(still_to_find, budget - 1, not_at_start);
}

void
NegativeLookaheadChoiceNode::GetQuickCheckDetails(QuickCheckDetails* details,
                                                  RegExpCompiler* compiler,
                                                  int filled_in,
                                                  bool not_at_start)
{
    // Alternative 0 is the negative lookahead, alternative 1 is what comes
    // afterwards.
    RegExpNode* node = alternatives()[1].node();
    return node->GetQuickCheckDetails(details, compiler, filled_in, not_at_start);
}

RegExpNode*
NegativeLookaheadChoiceNode::FilterASCII(int depth, bool ignore_case, bool unicode)
{
    if (info()->replacement_calculated)
        return replacement();
    if (depth < 0)
        return this;
    if (info()->visited)
        return this;

    VisitMarker marker(info());

    // Alternative 0 is the negative lookahead, alternative 1 is what comes
    // afterwards.
    RegExpNode* node = alternatives()[1].node();
    RegExpNode* replacement = node->FilterASCII(depth - 1, ignore_case, unicode);

    if (replacement == nullptr)
        return set_replacement(nullptr);
    alternatives()[1].set_node(replacement);

    RegExpNode* neg_node = alternatives()[0].node();
    RegExpNode* neg_replacement = neg_node->FilterASCII(depth - 1, ignore_case, unicode);

    // If the negative lookahead is always going to fail then
    // we don't need to check it.
    if (neg_replacement == nullptr)
        return set_replacement(replacement);

    alternatives()[0].set_node(neg_replacement);
    return set_replacement(this);
}

// -------------------------------------------------------------------
// LoopChoiceNode

void
GuardedAlternative::AddGuard(LifoAlloc* alloc, Guard* guard)
{
    if (guards_ == nullptr)
        guards_ = alloc->newInfallible<GuardVector>(*alloc);
    guards_->append(guard);
}

void
LoopChoiceNode::AddLoopAlternative(GuardedAlternative alt)
{
    MOZ_ASSERT(loop_node_ == nullptr);
    AddAlternative(alt);
    loop_node_ = alt.node();
}


void
LoopChoiceNode::AddContinueAlternative(GuardedAlternative alt)
{
    MOZ_ASSERT(continue_node_ == nullptr);
    AddAlternative(alt);
    continue_node_ = alt.node();
}

int
LoopChoiceNode::EatsAtLeast(int still_to_find,  int budget, bool not_at_start)
{
    return EatsAtLeastHelper(still_to_find,
                             budget - 1,
                             loop_node_,
                             not_at_start);
}

void
LoopChoiceNode::GetQuickCheckDetails(QuickCheckDetails* details,
                                     RegExpCompiler* compiler,
                                     int characters_filled_in,
                                     bool not_at_start)
{
    if (body_can_be_zero_length_ || info()->visited)
        return;
    VisitMarker marker(info());
    return ChoiceNode::GetQuickCheckDetails(details,
                                            compiler,
                                            characters_filled_in,
                                            not_at_start);
}

bool
LoopChoiceNode::FillInBMInfo(int offset,
                             int budget,
                             BoyerMooreLookahead* bm,
                             bool not_at_start)
{
    if (body_can_be_zero_length_ || budget <= 0) {
        bm->SetRest(offset);
        SaveBMInfo(bm, not_at_start, offset);
        return true;
    }
    if (!ChoiceNode::FillInBMInfo(offset, budget - 1, bm, not_at_start))
        return false;
    SaveBMInfo(bm, not_at_start, offset);
    return true;
}

RegExpNode*
LoopChoiceNode::FilterASCII(int depth, bool ignore_case, bool unicode)
{
    if (info()->replacement_calculated)
        return replacement();
    if (depth < 0)
        return this;
    if (info()->visited)
        return this;

    {
        VisitMarker marker(info());

        RegExpNode* continue_replacement =
            continue_node_->FilterASCII(depth - 1, ignore_case, unicode);

        // If we can't continue after the loop then there is no sense in doing the
        // loop.
        if (continue_replacement == nullptr)
            return set_replacement(nullptr);
    }

    return ChoiceNode::FilterASCII(depth - 1, ignore_case, unicode);
}

// -------------------------------------------------------------------
// Analysis

void
Analysis::EnsureAnalyzed(RegExpNode* that)
{
    if (!CheckRecursionLimit(cx)) {
        failASCII("Stack overflow");
        return;
    }

    if (that->info()->been_analyzed || that->info()->being_analyzed)
        return;
    that->info()->being_analyzed = true;
    that->Accept(this);
    that->info()->being_analyzed = false;
    that->info()->been_analyzed = true;
}

void
Analysis::VisitEnd(EndNode* that)
{
    // nothing to do
}

void
Analysis::VisitText(TextNode* that)
{
    if (ignore_case_)
        that->MakeCaseIndependent(is_ascii_, unicode_);
    EnsureAnalyzed(that->on_success());
    if (!has_failed()) {
        that->CalculateOffsets();
    }
}

void
Analysis::VisitAction(ActionNode* that)
{
    RegExpNode* target = that->on_success();
    EnsureAnalyzed(target);

    if (!has_failed()) {
        // If the next node is interested in what it follows then this node
        // has to be interested too so it can pass the information on.
        that->info()->AddFromFollowing(target->info());
    }
}

void
Analysis::VisitChoice(ChoiceNode* that)
{
    NodeInfo* info = that->info();

    for (size_t i = 0; i < that->alternatives().length(); i++) {
        RegExpNode* node = that->alternatives()[i].node();
        EnsureAnalyzed(node);
        if (has_failed()) return;

        // Anything the following nodes need to know has to be known by
        // this node also, so it can pass it on.
        info->AddFromFollowing(node->info());
    }
}

void
Analysis::VisitLoopChoice(LoopChoiceNode* that)
{
    NodeInfo* info = that->info();
    for (size_t i = 0; i < that->alternatives().length(); i++) {
        RegExpNode* node = that->alternatives()[i].node();
        if (node != that->loop_node()) {
            EnsureAnalyzed(node);
            if (has_failed()) return;
            info->AddFromFollowing(node->info());
        }
    }

    // Check the loop last since it may need the value of this node
    // to get a correct result.
    EnsureAnalyzed(that->loop_node());
    if (!has_failed())
        info->AddFromFollowing(that->loop_node()->info());
}

void
Analysis::VisitBackReference(BackReferenceNode* that)
{
    EnsureAnalyzed(that->on_success());
}

void
Analysis::VisitAssertion(AssertionNode* that)
{
    EnsureAnalyzed(that->on_success());
}

// -------------------------------------------------------------------
// Implementation of the Irregexp regular expression engine.
//
// The Irregexp regular expression engine is intended to be a complete
// implementation of ECMAScript regular expressions.  It generates either
// bytecodes or native code.

//   The Irregexp regexp engine is structured in three steps.
//   1) The parser generates an abstract syntax tree.  See RegExpAST.cpp.
//   2) From the AST a node network is created.  The nodes are all
//      subclasses of RegExpNode.  The nodes represent states when
//      executing a regular expression.  Several optimizations are
//      performed on the node network.
//   3) From the nodes we generate either byte codes or native code
//      that can actually execute the regular expression (perform
//      the search).  The code generation step is described in more
//      detail below.

// Code generation.
//
//   The nodes are divided into four main categories.
//   * Choice nodes
//        These represent places where the regular expression can
//        match in more than one way.  For example on entry to an
//        alternation (foo|bar) or a repetition (*, +, ? or {}).
//   * Action nodes
//        These represent places where some action should be
//        performed.  Examples include recording the current position
//        in the input string to a register (in order to implement
//        captures) or other actions on register for example in order
//        to implement the counters needed for {} repetitions.
//   * Matching nodes
//        These attempt to match some element part of the input string.
//        Examples of elements include character classes, plain strings
//        or back references.
//   * End nodes
//        These are used to implement the actions required on finding
//        a successful match or failing to find a match.
//
//   The code generated (whether as byte codes or native code) maintains
//   some state as it runs.  This consists of the following elements:
//
//   * The capture registers.  Used for string captures.
//   * Other registers.  Used for counters etc.
//   * The current position.
//   * The stack of backtracking information.  Used when a matching node
//     fails to find a match and needs to try an alternative.
//
// Conceptual regular expression execution model:
//
//   There is a simple conceptual model of regular expression execution
//   which will be presented first.  The actual code generated is a more
//   efficient simulation of the simple conceptual model:
//
//   * Choice nodes are implemented as follows:
//     For each choice except the last {
//       push current position
//       push backtrack code location
//       <generate code to test for choice>
//       backtrack code location:
//       pop current position
//     }
//     <generate code to test for last choice>
//
//   * Actions nodes are generated as follows
//     <push affected registers on backtrack stack>
//     <generate code to perform action>
//     push backtrack code location
//     <generate code to test for following nodes>
//     backtrack code location:
//     <pop affected registers to restore their state>
//     <pop backtrack location from stack and go to it>
//
//   * Matching nodes are generated as follows:
//     if input string matches at current position
//       update current position
//       <generate code to test for following nodes>
//     else
//       <pop backtrack location from stack and go to it>
//
//   Thus it can be seen that the current position is saved and restored
//   by the choice nodes, whereas the registers are saved and restored by
//   by the action nodes that manipulate them.
//
//   The other interesting aspect of this model is that nodes are generated
//   at the point where they are needed by a recursive call to Emit().  If
//   the node has already been code generated then the Emit() call will
//   generate a jump to the previously generated code instead.  In order to
//   limit recursion it is possible for the Emit() function to put the node
//   on a work list for later generation and instead generate a jump.  The
//   destination of the jump is resolved later when the code is generated.
//
// Actual regular expression code generation.
//
//   Code generation is actually more complicated than the above.  In order
//   to improve the efficiency of the generated code some optimizations are
//   performed
//
//   * Choice nodes have 1-character lookahead.
//     A choice node looks at the following character and eliminates some of
//     the choices immediately based on that character.  This is not yet
//     implemented.
//   * Simple greedy loops store reduced backtracking information.
//     A quantifier like /.*foo/m will greedily match the whole input.  It will
//     then need to backtrack to a point where it can match "foo".  The naive
//     implementation of this would push each character position onto the
//     backtracking stack, then pop them off one by one.  This would use space
//     proportional to the length of the input string.  However since the "."
//     can only match in one way and always has a constant length (in this case
//     of 1) it suffices to store the current position on the top of the stack
//     once.  Matching now becomes merely incrementing the current position and
//     backtracking becomes decrementing the current position and checking the
//     result against the stored current position.  This is faster and saves
//     space.
//   * The current state is virtualized.
//     This is used to defer expensive operations until it is clear that they
//     are needed and to generate code for a node more than once, allowing
//     specialized an efficient versions of the code to be created. This is
//     explained in the section below.
//
// Execution state virtualization.
//
//   Instead of emitting code, nodes that manipulate the state can record their
//   manipulation in an object called the Trace.  The Trace object can record a
//   current position offset, an optional backtrack code location on the top of
//   the virtualized backtrack stack and some register changes.  When a node is
//   to be emitted it can flush the Trace or update it.  Flushing the Trace
//   will emit code to bring the actual state into line with the virtual state.
//   Avoiding flushing the state can postpone some work (e.g. updates of capture
//   registers).  Postponing work can save time when executing the regular
//   expression since it may be found that the work never has to be done as a
//   failure to match can occur.  In addition it is much faster to jump to a
//   known backtrack code location than it is to pop an unknown backtrack
//   location from the stack and jump there.
//
//   The virtual state found in the Trace affects code generation.  For example
//   the virtual state contains the difference between the actual current
//   position and the virtual current position, and matching code needs to use
//   this offset to attempt a match in the correct location of the input
//   string.  Therefore code generated for a non-trivial trace is specialized
//   to that trace.  The code generator therefore has the ability to generate
//   code for each node several times.  In order to limit the size of the
//   generated code there is an arbitrary limit on how many specialized sets of
//   code may be generated for a given node.  If the limit is reached, the
//   trace is flushed and a generic version of the code for a node is emitted.
//   This is subsequently used for that node.  The code emitted for non-generic
//   trace is not recorded in the node and so it cannot currently be reused in
//   the event that code generation is requested for an identical trace.

/* static */ TextElement
TextElement::Atom(RegExpAtom* atom)
{
    return TextElement(ATOM, atom);
}

/* static */ TextElement
TextElement::CharClass(RegExpCharacterClass* char_class)
{
    return TextElement(CHAR_CLASS, char_class);
}

int
TextElement::length() const
{
    switch (text_type()) {
      case ATOM:
        return atom()->length();
      case CHAR_CLASS:
        return 1;
    }
    MOZ_CRASH("Bad text type");
}

class FrequencyCollator
{
  public:
    FrequencyCollator() : total_samples_(0) {
        for (int i = 0; i < RegExpMacroAssembler::kTableSize; i++) {
            frequencies_[i] = CharacterFrequency(i);
        }
    }

    void CountCharacter(int character) {
        int index = (character & RegExpMacroAssembler::kTableMask);
        frequencies_[index].Increment();
        total_samples_++;
    }

    // Does not measure in percent, but rather per-128 (the table size from the
    // regexp macro assembler).
    int Frequency(int in_character) {
        MOZ_ASSERT((in_character & RegExpMacroAssembler::kTableMask) == in_character);
        if (total_samples_ < 1) return 1;  // Division by zero.
        int freq_in_per128 =
            (frequencies_[in_character].counter() * 128) / total_samples_;
        return freq_in_per128;
    }

  private:
    class CharacterFrequency {
      public:
        CharacterFrequency() : counter_(0), character_(-1) { }
        explicit CharacterFrequency(int character)
          : counter_(0), character_(character)
        {}

        void Increment() { counter_++; }
        int counter() { return counter_; }
        int character() { return character_; }

     private:
        int counter_;
        int character_;
    };

  private:
    CharacterFrequency frequencies_[RegExpMacroAssembler::kTableSize];
    int total_samples_;
};

class irregexp::RegExpCompiler
{
  public:
    RegExpCompiler(JSContext* cx, LifoAlloc* alloc, int capture_count,
                   bool ignore_case, bool is_ascii, bool match_only, bool unicode);

    int AllocateRegister() {
        if (next_register_ >= RegExpMacroAssembler::kMaxRegister) {
            reg_exp_too_big_ = true;
            return next_register_;
        }
        return next_register_++;
    }

    RegExpCode Assemble(JSContext* cx,
                        RegExpMacroAssembler* assembler,
                        RegExpNode* start,
                        int capture_count);

    inline void AddWork(RegExpNode* node) {
        AutoEnterOOMUnsafeRegion oomUnsafe;
        if (!work_list_.append(node))
            oomUnsafe.crash("AddWork");
    }

    static const int kImplementationOffset = 0;
    static const int kNumberOfRegistersOffset = 0;
    static const int kCodeOffset = 1;

    RegExpMacroAssembler* macro_assembler() { return macro_assembler_; }
    EndNode* accept() { return accept_; }

    static const int kMaxRecursion = 100;
    inline int recursion_depth() { return recursion_depth_; }
    inline void IncrementRecursionDepth() { recursion_depth_++; }
    inline void DecrementRecursionDepth() { recursion_depth_--; }

    void SetRegExpTooBig() { reg_exp_too_big_ = true; }

    inline bool ignore_case() { return ignore_case_; }
    inline bool ascii() { return ascii_; }
    inline bool unicode() { return unicode_; }
    FrequencyCollator* frequency_collator() { return &frequency_collator_; }

    int current_expansion_factor() { return current_expansion_factor_; }
    void set_current_expansion_factor(int value) {
        current_expansion_factor_ = value;
    }

    JSContext* cx() const { return cx_; }
    LifoAlloc* alloc() const { return alloc_; }

    static const int kNoRegister = -1;

  private:
    EndNode* accept_;
    int next_register_;
    Vector<RegExpNode*, 4, SystemAllocPolicy> work_list_;
    int recursion_depth_;
    RegExpMacroAssembler* macro_assembler_;
    bool ignore_case_;
    bool ascii_;
    bool match_only_;
    bool unicode_;
    bool reg_exp_too_big_;
    int current_expansion_factor_;
    FrequencyCollator frequency_collator_;
    JSContext* cx_;
    LifoAlloc* alloc_;
};

class RecursionCheck
{
  public:
    explicit RecursionCheck(RegExpCompiler* compiler) : compiler_(compiler) {
        compiler->IncrementRecursionDepth();
    }
    ~RecursionCheck() { compiler_->DecrementRecursionDepth(); }

  private:
    RegExpCompiler* compiler_;
};

static inline bool
IsLatin1Equivalent(char16_t c, RegExpCompiler* compiler)
{
    if (c <= kMaxOneByteCharCode)
        return true;

    if (!compiler->ignore_case())
        return false;

    char16_t converted = ConvertNonLatin1ToLatin1(c, compiler->unicode());

    return converted != 0 && converted <= kMaxOneByteCharCode;
}

// Attempts to compile the regexp using an Irregexp code generator.  Returns
// a fixed array or a null handle depending on whether it succeeded.
RegExpCompiler::RegExpCompiler(JSContext* cx, LifoAlloc* alloc, int capture_count,
                               bool ignore_case, bool ascii, bool match_only, bool unicode)
  : next_register_(2 * (capture_count + 1)),
    recursion_depth_(0),
    ignore_case_(ignore_case),
    ascii_(ascii),
    match_only_(match_only),
    unicode_(unicode),
    reg_exp_too_big_(false),
    current_expansion_factor_(1),
    frequency_collator_(),
    cx_(cx),
    alloc_(alloc)
{
    accept_ = alloc->newInfallible<EndNode>(alloc, EndNode::ACCEPT);
    MOZ_ASSERT(next_register_ - 1 <= RegExpMacroAssembler::kMaxRegister);
}

RegExpCode
RegExpCompiler::Assemble(JSContext* cx,
                         RegExpMacroAssembler* assembler,
                         RegExpNode* start,
                         int capture_count)
{
    macro_assembler_ = assembler;
    macro_assembler_->set_slow_safe(false);

    // The LifoAlloc used by the regexp compiler is infallible and is currently
    // expected to crash on OOM. Thus we have to disable the assertions made to
    // prevent us from allocating any new chunk in the LifoAlloc. This is needed
    // because the jit::MacroAssembler turns these assertions on by default.
    LifoAlloc::AutoFallibleScope fallibleAllocator(alloc());

    jit::Label fail;
    macro_assembler_->PushBacktrack(&fail);
    Trace new_trace;
    start->Emit(this, &new_trace);
    macro_assembler_->BindBacktrack(&fail);
    macro_assembler_->Fail();

    while (!work_list_.empty())
        work_list_.popCopy()->Emit(this, &new_trace);

    RegExpCode code = macro_assembler_->GenerateCode(cx, match_only_);
    if (code.empty())
        return RegExpCode();

    if (reg_exp_too_big_) {
        code.destroy();
        JS_ReportErrorASCII(cx, "regexp too big");
        return RegExpCode();
    }

    return code;
}

template <typename CharT>
static void
SampleChars(FrequencyCollator* collator, const CharT* chars, size_t length)
{
    // Sample some characters from the middle of the string.
    static const int kSampleSize = 128;

    int chars_sampled = 0;
    int half_way = (int(length) - kSampleSize) / 2;
    for (size_t i = Max(0, half_way);
         i < length && chars_sampled < kSampleSize;
         i++, chars_sampled++)
    {
        collator->CountCharacter(chars[i]);
    }
}

static bool
IsNativeRegExpEnabled(JSContext* cx)
{
#ifdef JS_CODEGEN_NONE
    return false;
#else
    return cx->options().nativeRegExp();
#endif
}

RegExpCode
irregexp::CompilePattern(JSContext* cx, HandleRegExpShared shared, RegExpCompileData* data,
                         HandleLinearString sample, bool is_global, bool ignore_case,
                         bool is_ascii, bool match_only, bool force_bytecode, bool sticky,
                         bool unicode)
{
    if ((data->capture_count + 1) * 2 - 1 > RegExpMacroAssembler::kMaxRegister) {
        JS_ReportErrorASCII(cx, "regexp too big");
        return RegExpCode();
    }

    LifoAlloc& alloc = cx->tempLifoAlloc();
    RegExpCompiler compiler(cx, &alloc, data->capture_count, ignore_case, is_ascii, match_only,
                            unicode);

    // Sample some characters from the middle of the string.
    if (sample->hasLatin1Chars()) {
        JS::AutoCheckCannotGC nogc;
        SampleChars(compiler.frequency_collator(), sample->latin1Chars(nogc), sample->length());
    } else {
        JS::AutoCheckCannotGC nogc;
        SampleChars(compiler.frequency_collator(), sample->twoByteChars(nogc), sample->length());
    }

    // Wrap the body of the regexp in capture #0.
    RegExpNode* captured_body = RegExpCapture::ToNode(data->tree,
                                                      0,
                                                      &compiler,
                                                      compiler.accept());
    RegExpNode* node = captured_body;
    bool is_end_anchored = data->tree->IsAnchoredAtEnd();
    bool is_start_anchored = sticky || data->tree->IsAnchoredAtStart();
    int max_length = data->tree->max_match();
    if (!is_start_anchored) {
        // Add a .*? at the beginning, outside the body capture, unless
        // this expression is anchored at the beginning.
        RegExpNode* loop_node =
            RegExpQuantifier::ToNode(0,
                                     RegExpTree::kInfinity,
                                     false,
                                     alloc.newInfallible<RegExpCharacterClass>('*'),
                                     &compiler,
                                     captured_body,
                                     data->contains_anchor);

        if (data->contains_anchor) {
            // Unroll loop once, to take care of the case that might start
            // at the start of input.
            ChoiceNode* first_step_node = alloc.newInfallible<ChoiceNode>(&alloc, 2);
            RegExpNode* char_class =
                alloc.newInfallible<TextNode>(alloc.newInfallible<RegExpCharacterClass>('*'), loop_node);
            first_step_node->AddAlternative(GuardedAlternative(captured_body));
            first_step_node->AddAlternative(GuardedAlternative(char_class));
            node = first_step_node;
        } else {
            node = loop_node;
        }
    }
    if (is_ascii) {
        node = node->FilterASCII(RegExpCompiler::kMaxRecursion, ignore_case, unicode);
        // Do it again to propagate the new nodes to places where they were not
        // put because they had not been calculated yet.
        if (node != nullptr) {
            node = node->FilterASCII(RegExpCompiler::kMaxRecursion, ignore_case, unicode);
        }
    }

    if (node == nullptr)
        node = alloc.newInfallible<EndNode>(&alloc, EndNode::BACKTRACK);

    Analysis analysis(cx, ignore_case, is_ascii, unicode);
    analysis.EnsureAnalyzed(node);
    if (analysis.has_failed()) {
        JS_ReportErrorASCII(cx, "%s", analysis.errorMessage());
        return RegExpCode();
    }

    Maybe<jit::JitContext> ctx;
    Maybe<NativeRegExpMacroAssembler> native_assembler;
    Maybe<InterpretedRegExpMacroAssembler> interpreted_assembler;

    RegExpMacroAssembler* assembler;
    if (IsNativeRegExpEnabled(cx) &&
        !force_bytecode &&
        jit::CanLikelyAllocateMoreExecutableMemory() &&
        shared->getSource()->length() < 32 * 1024)
    {
        NativeRegExpMacroAssembler::Mode mode =
            is_ascii ? NativeRegExpMacroAssembler::ASCII
                     : NativeRegExpMacroAssembler::CHAR16;

        ctx.emplace(cx, (jit::TempAllocator*) nullptr);
        native_assembler.emplace(cx, &alloc, shared, mode, (data->capture_count + 1) * 2);
        assembler = native_assembler.ptr();
    } else {
        interpreted_assembler.emplace(&alloc, shared, (data->capture_count + 1) * 2);
        assembler = interpreted_assembler.ptr();
    }

    // Inserted here, instead of in Assembler, because it depends on information
    // in the AST that isn't replicated in the Node structure.
    static const int kMaxBacksearchLimit = 1024;
    if (is_end_anchored &&
        !is_start_anchored &&
        max_length < kMaxBacksearchLimit) {
        assembler->SetCurrentPositionFromEnd(max_length);
    }

    if (is_global) {
        assembler->set_global_mode((data->tree->min_match() > 0)
                                   ? RegExpMacroAssembler::GLOBAL_NO_ZERO_LENGTH_CHECK
                                   : RegExpMacroAssembler::GLOBAL);
    }

    return compiler.Assemble(cx, assembler, node, data->capture_count);
}

template <typename CharT>
RegExpRunStatus
irregexp::ExecuteCode(JSContext* cx, jit::JitCode* codeBlock, const CharT* chars, size_t start,
                      size_t length, MatchPairs* matches, size_t* endIndex)
{
    typedef void (*RegExpCodeSignature)(InputOutputData*);

    InputOutputData data(chars, chars + length, start, matches, endIndex);

    RegExpCodeSignature function = reinterpret_cast<RegExpCodeSignature>(codeBlock->raw());

    {
        JS::AutoSuppressGCAnalysis nogc;
        CALL_GENERATED_1(function, &data);
    }

    return (RegExpRunStatus) data.result;
}

template RegExpRunStatus
irregexp::ExecuteCode(JSContext* cx, jit::JitCode* codeBlock, const Latin1Char* chars, size_t start,
                      size_t length, MatchPairs* matches, size_t* endIndex);

template RegExpRunStatus
irregexp::ExecuteCode(JSContext* cx, jit::JitCode* codeBlock, const char16_t* chars, size_t start,
                      size_t length, MatchPairs* matches, size_t* endIndex);

// -------------------------------------------------------------------
// Tree to graph conversion

RegExpNode*
RegExpAtom::ToNode(RegExpCompiler* compiler, RegExpNode* on_success)
{
    TextElementVector* elms =
        compiler->alloc()->newInfallible<TextElementVector>(*compiler->alloc());
    elms->append(TextElement::Atom(this));
    return compiler->alloc()->newInfallible<TextNode>(elms, on_success);
}

RegExpNode*
RegExpText::ToNode(RegExpCompiler* compiler, RegExpNode* on_success)
{
    return compiler->alloc()->newInfallible<TextNode>(&elements_, on_success);
}

RegExpNode*
RegExpCharacterClass::ToNode(RegExpCompiler* compiler, RegExpNode* on_success)
{
    return compiler->alloc()->newInfallible<TextNode>(this, on_success);
}

RegExpNode*
RegExpDisjunction::ToNode(RegExpCompiler* compiler, RegExpNode* on_success)
{
    const RegExpTreeVector& alternatives = this->alternatives();
    size_t length = alternatives.length();
    ChoiceNode* result = compiler->alloc()->newInfallible<ChoiceNode>(compiler->alloc(), length);
    for (size_t i = 0; i < length; i++) {
        GuardedAlternative alternative(alternatives[i]->ToNode(compiler, on_success));
        result->AddAlternative(alternative);
    }
    return result;
}

RegExpNode*
RegExpQuantifier::ToNode(RegExpCompiler* compiler, RegExpNode* on_success)
{
    return ToNode(min(),
                  max(),
                  is_greedy(),
                  body(),
                  compiler,
                  on_success);
}

// Scoped object to keep track of how much we unroll quantifier loops in the
// regexp graph generator.
class RegExpExpansionLimiter
{
  public:
    static const int kMaxExpansionFactor = 6;
    RegExpExpansionLimiter(RegExpCompiler* compiler, int factor)
      : compiler_(compiler),
        saved_expansion_factor_(compiler->current_expansion_factor()),
        ok_to_expand_(saved_expansion_factor_ <= kMaxExpansionFactor)
    {
        MOZ_ASSERT(factor > 0);
        if (ok_to_expand_) {
            if (factor > kMaxExpansionFactor) {
                // Avoid integer overflow of the current expansion factor.
                ok_to_expand_ = false;
                compiler->set_current_expansion_factor(kMaxExpansionFactor + 1);
            } else {
                int new_factor = saved_expansion_factor_ * factor;
                ok_to_expand_ = (new_factor <= kMaxExpansionFactor);
                compiler->set_current_expansion_factor(new_factor);
            }
        }
    }

    ~RegExpExpansionLimiter() {
        compiler_->set_current_expansion_factor(saved_expansion_factor_);
    }

    bool ok_to_expand() { return ok_to_expand_; }

  private:
    RegExpCompiler* compiler_;
    int saved_expansion_factor_;
    bool ok_to_expand_;
};

/* static */ RegExpNode*
RegExpQuantifier::ToNode(int min,
                         int max,
                         bool is_greedy,
                         RegExpTree* body,
                         RegExpCompiler* compiler,
                         RegExpNode* on_success,
                         bool not_at_start /* = false */)
{
    // x{f, t} becomes this:
    //
    //             (r++)<-.
    //               |     `
    //               |     (x)
    //               v     ^
    //      (r=0)-->(?)---/ [if r < t]
    //               |
    //   [if r >= f] \----> ...
    //

    // 15.10.2.5 RepeatMatcher algorithm.
    // The parser has already eliminated the case where max is 0.  In the case
    // where max_match is zero the parser has removed the quantifier if min was
    // > 0 and removed the atom if min was 0.  See AddQuantifierToAtom.

    // If we know that we cannot match zero length then things are a little
    // simpler since we don't need to make the special zero length match check
    // from step 2.1.  If the min and max are small we can unroll a little in
    // this case.
    static const int kMaxUnrolledMinMatches = 3;  // Unroll (foo)+ and (foo){3,}
    static const int kMaxUnrolledMaxMatches = 3;  // Unroll (foo)? and (foo){x,3}

    if (max == 0)
        return on_success;  // This can happen due to recursion.

    bool body_can_be_empty = (body->min_match() == 0);
    int body_start_reg = RegExpCompiler::kNoRegister;
    Interval capture_registers = body->CaptureRegisters();
    bool needs_capture_clearing = !capture_registers.is_empty();
    LifoAlloc* alloc = compiler->alloc();

    if (body_can_be_empty) {
        body_start_reg = compiler->AllocateRegister();
    } else if (!needs_capture_clearing) {
        // Only unroll if there are no captures and the body can't be
        // empty.
        {
            RegExpExpansionLimiter limiter(compiler, min + ((max != min) ? 1 : 0));
            if (min > 0 && min <= kMaxUnrolledMinMatches && limiter.ok_to_expand()) {
                int new_max = (max == kInfinity) ? max : max - min;
                // Recurse once to get the loop or optional matches after the fixed
                // ones.
                RegExpNode* answer = ToNode(0, new_max, is_greedy, body, compiler, on_success, true);
                // Unroll the forced matches from 0 to min.  This can cause chains of
                // TextNodes (which the parser does not generate).  These should be
                // combined if it turns out they hinder good code generation.
                for (int i = 0; i < min; i++)
                    answer = body->ToNode(compiler, answer);
                return answer;
            }
        }
        if (max <= kMaxUnrolledMaxMatches && min == 0) {
            MOZ_ASSERT(max > 0);  // Due to the 'if' above.
            RegExpExpansionLimiter limiter(compiler, max);
            if (limiter.ok_to_expand()) {
                // Unroll the optional matches up to max.
                RegExpNode* answer = on_success;
                for (int i = 0; i < max; i++) {
                    ChoiceNode* alternation = alloc->newInfallible<ChoiceNode>(alloc, 2);
                    if (is_greedy) {
                        alternation->AddAlternative(GuardedAlternative(body->ToNode(compiler, answer)));
                        alternation->AddAlternative(GuardedAlternative(on_success));
                    } else {
                        alternation->AddAlternative(GuardedAlternative(on_success));
                        alternation->AddAlternative(GuardedAlternative(body->ToNode(compiler, answer)));
                    }
                    answer = alternation;
                    if (not_at_start) alternation->set_not_at_start();
                }
                return answer;
            }
        }
    }
    bool has_min = min > 0;
    bool has_max = max < RegExpTree::kInfinity;
    bool needs_counter = has_min || has_max;
    int reg_ctr = needs_counter
        ? compiler->AllocateRegister()
        : RegExpCompiler::kNoRegister;
    LoopChoiceNode* center = alloc->newInfallible<LoopChoiceNode>(alloc, body->min_match() == 0);
    if (not_at_start)
        center->set_not_at_start();
    RegExpNode* loop_return = needs_counter
        ? static_cast<RegExpNode*>(ActionNode::IncrementRegister(reg_ctr, center))
        : static_cast<RegExpNode*>(center);
    if (body_can_be_empty) {
        // If the body can be empty we need to check if it was and then
        // backtrack.
        loop_return = ActionNode::EmptyMatchCheck(body_start_reg,
                                                  reg_ctr,
                                                  min,
                                                  loop_return);
    }
    RegExpNode* body_node = body->ToNode(compiler, loop_return);
    if (body_can_be_empty) {
        // If the body can be empty we need to store the start position
        // so we can bail out if it was empty.
        body_node = ActionNode::StorePosition(body_start_reg, false, body_node);
    }
    if (needs_capture_clearing) {
        // Before entering the body of this loop we need to clear captures.
        body_node = ActionNode::ClearCaptures(capture_registers, body_node);
    }
    GuardedAlternative body_alt(body_node);
    if (has_max) {
        Guard* body_guard = alloc->newInfallible<Guard>(reg_ctr, Guard::LT, max);
        body_alt.AddGuard(alloc, body_guard);
    }
    GuardedAlternative rest_alt(on_success);
    if (has_min) {
        Guard* rest_guard = alloc->newInfallible<Guard>(reg_ctr, Guard::GEQ, min);
        rest_alt.AddGuard(alloc, rest_guard);
    }
    if (is_greedy) {
        center->AddLoopAlternative(body_alt);
        center->AddContinueAlternative(rest_alt);
    } else {
        center->AddContinueAlternative(rest_alt);
        center->AddLoopAlternative(body_alt);
    }
    if (needs_counter)
        return ActionNode::SetRegister(reg_ctr, 0, center);
    return center;
}

RegExpNode*
RegExpAssertion::ToNode(RegExpCompiler* compiler,
                        RegExpNode* on_success)
{
    NodeInfo info;
    LifoAlloc* alloc = compiler->alloc();

    switch (assertion_type()) {
      case START_OF_LINE:
        return AssertionNode::AfterNewline(on_success);
      case START_OF_INPUT:
        return AssertionNode::AtStart(on_success);
      case BOUNDARY:
        return AssertionNode::AtBoundary(on_success);
      case NON_BOUNDARY:
        return AssertionNode::AtNonBoundary(on_success);
      case END_OF_INPUT:
        return AssertionNode::AtEnd(on_success);
      case END_OF_LINE: {
        // Compile $ in multiline regexps as an alternation with a positive
        // lookahead in one side and an end-of-input on the other side.
        // We need two registers for the lookahead.
        int stack_pointer_register = compiler->AllocateRegister();
        int position_register = compiler->AllocateRegister();
        // The ChoiceNode to distinguish between a newline and end-of-input.
        ChoiceNode* result = alloc->newInfallible<ChoiceNode>(alloc, 2);
        // Create a newline atom.
        CharacterRangeVector* newline_ranges = alloc->newInfallible<CharacterRangeVector>(*alloc);
        CharacterRange::AddClassEscape(alloc, 'n', newline_ranges);
        RegExpCharacterClass* newline_atom = alloc->newInfallible<RegExpCharacterClass>('n');
        TextNode* newline_matcher =
            alloc->newInfallible<TextNode>(newline_atom,
                ActionNode::PositiveSubmatchSuccess(stack_pointer_register,
                                                    position_register,
                                                    0,  // No captures inside.
                                                    -1,  // Ignored if no captures.
                                                    on_success));
        // Create an end-of-input matcher.
        RegExpNode* end_of_line =
            ActionNode::BeginSubmatch(stack_pointer_register, position_register, newline_matcher);

        // Add the two alternatives to the ChoiceNode.
        GuardedAlternative eol_alternative(end_of_line);
        result->AddAlternative(eol_alternative);
        GuardedAlternative end_alternative(AssertionNode::AtEnd(on_success));
        result->AddAlternative(end_alternative);
        return result;
      }
      case NOT_AFTER_LEAD_SURROGATE:
        return AssertionNode::NotAfterLeadSurrogate(on_success);
      case NOT_IN_SURROGATE_PAIR:
        return AssertionNode::NotInSurrogatePair(on_success);
      default:
        MOZ_CRASH("Bad assertion type");
    }
    return on_success;
}

RegExpNode*
RegExpBackReference::ToNode(RegExpCompiler* compiler, RegExpNode* on_success)
{
    return compiler->alloc()->newInfallible<BackReferenceNode>(RegExpCapture::StartRegister(index()),
                                                               RegExpCapture::EndRegister(index()),
                                                               on_success);
}

RegExpNode*
RegExpEmpty::ToNode(RegExpCompiler* compiler, RegExpNode* on_success)
{
    return on_success;
}

RegExpNode*
RegExpLookahead::ToNode(RegExpCompiler* compiler, RegExpNode* on_success)
{
    int stack_pointer_register = compiler->AllocateRegister();
    int position_register = compiler->AllocateRegister();

    const int registers_per_capture = 2;
    const int register_of_first_capture = 2;
    int register_count = capture_count_ * registers_per_capture;
    int register_start =
        register_of_first_capture + capture_from_ * registers_per_capture;

    if (is_positive()) {
        RegExpNode* bodyNode =
            body()->ToNode(compiler,
                           ActionNode::PositiveSubmatchSuccess(stack_pointer_register,
                                                               position_register,
                                                               register_count,
                                                               register_start,
                                                               on_success));
        return ActionNode::BeginSubmatch(stack_pointer_register,
                                         position_register,
                                         bodyNode);
    }

    // We use a ChoiceNode for a negative lookahead because it has most of
    // the characteristics we need.  It has the body of the lookahead as its
    // first alternative and the expression after the lookahead of the second
    // alternative.  If the first alternative succeeds then the
    // NegativeSubmatchSuccess will unwind the stack including everything the
    // choice node set up and backtrack.  If the first alternative fails then
    // the second alternative is tried, which is exactly the desired result
    // for a negative lookahead.  The NegativeLookaheadChoiceNode is a special
    // ChoiceNode that knows to ignore the first exit when calculating quick
    // checks.
    LifoAlloc* alloc = compiler->alloc();

    RegExpNode* success =
        alloc->newInfallible<NegativeSubmatchSuccess>(alloc,
                                                      stack_pointer_register,
                                                      position_register,
                                                      register_count,
                                                      register_start);
    GuardedAlternative body_alt(body()->ToNode(compiler, success));

    ChoiceNode* choice_node =
        alloc->newInfallible<NegativeLookaheadChoiceNode>(alloc, body_alt, GuardedAlternative(on_success));

    return ActionNode::BeginSubmatch(stack_pointer_register,
                                     position_register,
                                     choice_node);
}

RegExpNode*
RegExpCapture::ToNode(RegExpCompiler* compiler, RegExpNode* on_success)
{
    return ToNode(body(), index(), compiler, on_success);
}

/* static */ RegExpNode*
RegExpCapture::ToNode(RegExpTree* body,
                      int index,
                      RegExpCompiler* compiler,
                      RegExpNode* on_success)
{
    int start_reg = RegExpCapture::StartRegister(index);
    int end_reg = RegExpCapture::EndRegister(index);
    RegExpNode* store_end = ActionNode::StorePosition(end_reg, true, on_success);
    RegExpNode* body_node = body->ToNode(compiler, store_end);
    return ActionNode::StorePosition(start_reg, true, body_node);
}

RegExpNode*
RegExpAlternative::ToNode(RegExpCompiler* compiler, RegExpNode* on_success)
{
    const RegExpTreeVector& children = nodes();
    RegExpNode* current = on_success;
    for (int i = children.length() - 1; i >= 0; i--)
        current = children[i]->ToNode(compiler, current);
    return current;
}

// -------------------------------------------------------------------
// BoyerMooreLookahead

ContainedInLattice
irregexp::AddRange(ContainedInLattice containment,
                   const int* ranges,
                   int ranges_length,
                   Interval new_range)
{
    MOZ_ASSERT((ranges_length & 1) == 1);
    MOZ_ASSERT(ranges[ranges_length - 1] == kMaxUtf16CodeUnit + 1);
    if (containment == kLatticeUnknown) return containment;
    bool inside = false;
    int last = 0;
    for (int i = 0; i < ranges_length; inside = !inside, last = ranges[i], i++) {
        // Consider the range from last to ranges[i].
        // We haven't got to the new range yet.
        if (ranges[i] <= new_range.from())
            continue;

        // New range is wholly inside last-ranges[i].  Note that new_range.to() is
        // inclusive, but the values in ranges are not.
        if (last <= new_range.from() && new_range.to() < ranges[i])
            return Combine(containment, inside ? kLatticeIn : kLatticeOut);

        return kLatticeUnknown;
    }
    return containment;
}

void
BoyerMoorePositionInfo::Set(int character)
{
    SetInterval(Interval(character, character));
}

void
BoyerMoorePositionInfo::SetInterval(const Interval& interval)
{
    s_ = AddRange(s_, kSpaceRanges, kSpaceRangeCount, interval);
    if (unicode_ignore_case_)
        w_ = AddRange(w_, kIgnoreCaseWordRanges, kIgnoreCaseWordRangeCount, interval);
    else
        w_ = AddRange(w_, kWordRanges, kWordRangeCount, interval);
    d_ = AddRange(d_, kDigitRanges, kDigitRangeCount, interval);
    surrogate_ =
        AddRange(surrogate_, kSurrogateRanges, kSurrogateRangeCount, interval);
    if (interval.to() - interval.from() >= kMapSize - 1) {
        if (map_count_ != kMapSize) {
            map_count_ = kMapSize;
            for (int i = 0; i < kMapSize; i++)
                map_[i] = true;
        }
        return;
    }
    for (int i = interval.from(); i <= interval.to(); i++) {
        int mod_character = (i & kMask);
        if (!map_[mod_character]) {
            map_count_++;
            map_[mod_character] = true;
        }
        if (map_count_ == kMapSize)
            return;
    }
}

void
BoyerMoorePositionInfo::SetAll()
{
    s_ = w_ = d_ = kLatticeUnknown;
    if (map_count_ != kMapSize) {
        map_count_ = kMapSize;
        for (int i = 0; i < kMapSize; i++)
            map_[i] = true;
    }
}

BoyerMooreLookahead::BoyerMooreLookahead(LifoAlloc* alloc, size_t length, RegExpCompiler* compiler)
  : length_(length), compiler_(compiler), bitmaps_(*alloc)
{
    bool unicode_ignore_case = compiler->unicode() && compiler->ignore_case();
    max_char_ = MaximumCharacter(compiler->ascii());

    bitmaps_.reserve(length);
    for (size_t i = 0; i < length; i++)
        bitmaps_.append(alloc->newInfallible<BoyerMoorePositionInfo>(alloc, unicode_ignore_case));
}

// Find the longest range of lookahead that has the fewest number of different
// characters that can occur at a given position.  Since we are optimizing two
// different parameters at once this is a tradeoff.
bool BoyerMooreLookahead::FindWorthwhileInterval(int* from, int* to) {
  int biggest_points = 0;
  // If more than 32 characters out of 128 can occur it is unlikely that we can
  // be lucky enough to step forwards much of the time.
  const int kMaxMax = 32;
  for (int max_number_of_chars = 4;
       max_number_of_chars < kMaxMax;
       max_number_of_chars *= 2) {
    biggest_points =
        FindBestInterval(max_number_of_chars, biggest_points, from, to);
  }
  if (biggest_points == 0) return false;
  return true;
}

// Find the highest-points range between 0 and length_ where the character
// information is not too vague.  'Too vague' means that there are more than
// max_number_of_chars that can occur at this position.  Calculates the number
// of points as the product of width-of-the-range and
// probability-of-finding-one-of-the-characters, where the probability is
// calculated using the frequency distribution of the sample subject string.
int
BoyerMooreLookahead::FindBestInterval(int max_number_of_chars, int old_biggest_points,
                                      int* from, int* to)
{
    int biggest_points = old_biggest_points;
    static const int kSize = RegExpMacroAssembler::kTableSize;
    for (int i = 0; i < length_; ) {
        while (i < length_ && Count(i) > max_number_of_chars) i++;
        if (i == length_) break;
        int remembered_from = i;
        bool union_map[kSize];
        for (int j = 0; j < kSize; j++) union_map[j] = false;
        while (i < length_ && Count(i) <= max_number_of_chars) {
            BoyerMoorePositionInfo* map = bitmaps_[i];
            for (int j = 0; j < kSize; j++) union_map[j] |= map->at(j);
            i++;
        }
        int frequency = 0;
        for (int j = 0; j < kSize; j++) {
            if (union_map[j]) {
                // Add 1 to the frequency to give a small per-character boost for
                // the cases where our sampling is not good enough and many
                // characters have a frequency of zero.  This means the frequency
                // can theoretically be up to 2*kSize though we treat it mostly as
                // a fraction of kSize.
                frequency += compiler_->frequency_collator()->Frequency(j) + 1;
            }
        }
        // We use the probability of skipping times the distance we are skipping to
        // judge the effectiveness of this.  Actually we have a cut-off:  By
        // dividing by 2 we switch off the skipping if the probability of skipping
        // is less than 50%.  This is because the multibyte mask-and-compare
        // skipping in quickcheck is more likely to do well on this case.
        bool in_quickcheck_range = ((i - remembered_from < 4) ||
                                    (compiler_->ascii() ? remembered_from <= 4 : remembered_from <= 2));
        // Called 'probability' but it is only a rough estimate and can actually
        // be outside the 0-kSize range.
        int probability = (in_quickcheck_range ? kSize / 2 : kSize) - frequency;
        int points = (i - remembered_from) * probability;
        if (points > biggest_points) {
            *from = remembered_from;
            *to = i - 1;
            biggest_points = points;
        }
    }
    return biggest_points;
}

// Take all the characters that will not prevent a successful match if they
// occur in the subject string in the range between min_lookahead and
// max_lookahead (inclusive) measured from the current position.  If the
// character at max_lookahead offset is not one of these characters, then we
// can safely skip forwards by the number of characters in the range.
int BoyerMooreLookahead::GetSkipTable(int min_lookahead,
                                      int max_lookahead,
                                      uint8_t* boolean_skip_table)
{
    const int kSize = RegExpMacroAssembler::kTableSize;

    const int kSkipArrayEntry = 0;
    const int kDontSkipArrayEntry = 1;

    for (int i = 0; i < kSize; i++)
        boolean_skip_table[i] = kSkipArrayEntry;
    int skip = max_lookahead + 1 - min_lookahead;

    for (int i = max_lookahead; i >= min_lookahead; i--) {
        BoyerMoorePositionInfo* map = bitmaps_[i];
        for (int j = 0; j < kSize; j++) {
            if (map->at(j))
                boolean_skip_table[j] = kDontSkipArrayEntry;
        }
    }

    return skip;
}

// See comment on the implementation of GetSkipTable.
bool
BoyerMooreLookahead::EmitSkipInstructions(RegExpMacroAssembler* masm)
{
    const int kSize = RegExpMacroAssembler::kTableSize;

    int min_lookahead = 0;
    int max_lookahead = 0;

    if (!FindWorthwhileInterval(&min_lookahead, &max_lookahead))
        return false;

    bool found_single_character = false;
    int single_character = 0;
    for (int i = max_lookahead; i >= min_lookahead; i--) {
        BoyerMoorePositionInfo* map = bitmaps_[i];
        if (map->map_count() > 1 ||
            (found_single_character && map->map_count() != 0)) {
            found_single_character = false;
            break;
        }
        for (int j = 0; j < kSize; j++) {
            if (map->at(j)) {
                found_single_character = true;
                single_character = j;
                break;
            }
        }
    }

    int lookahead_width = max_lookahead + 1 - min_lookahead;

    if (found_single_character && lookahead_width == 1 && max_lookahead < 3) {
        // The mask-compare can probably handle this better.
        return false;
    }

    if (found_single_character) {
        jit::Label cont, again;
        masm->Bind(&again);
        masm->LoadCurrentCharacter(max_lookahead, &cont, true);
        if (max_char_ > kSize) {
            masm->CheckCharacterAfterAnd(single_character,
                                         RegExpMacroAssembler::kTableMask,
                                         &cont);
        } else {
            masm->CheckCharacter(single_character, &cont);
        }
        masm->AdvanceCurrentPosition(lookahead_width);
        masm->JumpOrBacktrack(&again);
        masm->Bind(&cont);
        return true;
    }

    uint8_t* boolean_skip_table;
    {
        AutoEnterOOMUnsafeRegion oomUnsafe;
        boolean_skip_table = static_cast<uint8_t*>(js_malloc(kSize));
        if (!boolean_skip_table || !masm->shared->addTable(boolean_skip_table))
            oomUnsafe.crash("Table malloc");
    }

    int skip_distance = GetSkipTable(min_lookahead, max_lookahead, boolean_skip_table);
    MOZ_ASSERT(skip_distance != 0);

    jit::Label cont, again;
    masm->Bind(&again);
    masm->LoadCurrentCharacter(max_lookahead, &cont, true);
    masm->CheckBitInTable(boolean_skip_table, &cont);
    masm->AdvanceCurrentPosition(skip_distance);
    masm->JumpOrBacktrack(&again);
    masm->Bind(&cont);

    return true;
}

bool
BoyerMooreLookahead::CheckOverRecursed()
{
    if (!CheckRecursionLimit(compiler()->cx())) {
        compiler()->SetRegExpTooBig();
        return false;
    }

    return true;
}

// -------------------------------------------------------------------
// Trace

bool Trace::DeferredAction::Mentions(int that)
{
    if (action_type() == ActionNode::CLEAR_CAPTURES) {
        Interval range = static_cast<DeferredClearCaptures*>(this)->range();
        return range.Contains(that);
    }
    return reg() == that;
}

bool Trace::mentions_reg(int reg)
{
    for (DeferredAction* action = actions_; action != nullptr; action = action->next()) {
        if (action->Mentions(reg))
            return true;
    }
    return false;
}

bool
Trace::GetStoredPosition(int reg, int* cp_offset)
{
    MOZ_ASSERT(0 == *cp_offset);
    for (DeferredAction* action = actions_; action != nullptr; action = action->next()) {
        if (action->Mentions(reg)) {
            if (action->action_type() == ActionNode::STORE_POSITION) {
                *cp_offset = static_cast<DeferredCapture*>(action)->cp_offset();
                return true;
            }
            return false;
        }
    }
    return false;
}

int
Trace::FindAffectedRegisters(LifoAlloc* alloc, OutSet* affected_registers)
{
    int max_register = RegExpCompiler::kNoRegister;
    for (DeferredAction* action = actions_; action != nullptr; action = action->next()) {
        if (action->action_type() == ActionNode::CLEAR_CAPTURES) {
            Interval range = static_cast<DeferredClearCaptures*>(action)->range();
            for (int i = range.from(); i <= range.to(); i++)
                affected_registers->Set(alloc, i);
            if (range.to() > max_register) max_register = range.to();
        } else {
            affected_registers->Set(alloc, action->reg());
            if (action->reg() > max_register) max_register = action->reg();
        }
    }
    return max_register;
}

void
Trace::RestoreAffectedRegisters(RegExpMacroAssembler* assembler,
                                int max_register,
                                OutSet& registers_to_pop,
                                OutSet& registers_to_clear)
{
    for (int reg = max_register; reg >= 0; reg--) {
        if (registers_to_pop.Get(reg)) assembler->PopRegister(reg);
        else if (registers_to_clear.Get(reg)) {
            int clear_to = reg;
            while (reg > 0 && registers_to_clear.Get(reg - 1))
                reg--;
            assembler->ClearRegisters(reg, clear_to);
        }
    }
}

enum DeferredActionUndoType {
    DEFER_IGNORE,
    DEFER_RESTORE,
    DEFER_CLEAR
};

void
Trace::PerformDeferredActions(LifoAlloc* alloc,
                              RegExpMacroAssembler* assembler,
                              int max_register,
                              OutSet& affected_registers,
                              OutSet* registers_to_pop,
                              OutSet* registers_to_clear)
{
    // The "+1" is to avoid a push_limit of zero if stack_limit_slack() is 1.
    const int push_limit = (assembler->stack_limit_slack() + 1) / 2;

    // Count pushes performed to force a stack limit check occasionally.
    int pushes = 0;

    for (int reg = 0; reg <= max_register; reg++) {
        if (!affected_registers.Get(reg))
            continue;

        // The chronologically first deferred action in the trace
        // is used to infer the action needed to restore a register
        // to its previous state (or not, if it's safe to ignore it).
        DeferredActionUndoType undo_action = DEFER_IGNORE;

        int value = 0;
        bool absolute = false;
        bool clear = false;
        int store_position = -1;
        // This is a little tricky because we are scanning the actions in reverse
        // historical order (newest first).
        for (DeferredAction* action = actions_;
             action != nullptr;
             action = action->next()) {
            if (action->Mentions(reg)) {
                switch (action->action_type()) {
                  case ActionNode::SET_REGISTER: {
                    Trace::DeferredSetRegister* psr =
                        static_cast<Trace::DeferredSetRegister*>(action);
                    if (!absolute) {
                        value += psr->value();
                        absolute = true;
                    }
                    // SET_REGISTER is currently only used for newly introduced loop
                    // counters. They can have a significant previous value if they
                    // occour in a loop. TODO(lrn): Propagate this information, so
                    // we can set undo_action to IGNORE if we know there is no value to
                    // restore.
                    undo_action = DEFER_RESTORE;
                    MOZ_ASSERT(store_position == -1);
                    MOZ_ASSERT(!clear);
                    break;
                  }
                  case ActionNode::INCREMENT_REGISTER:
                    if (!absolute) {
                        value++;
                    }
                    MOZ_ASSERT(store_position == -1);
                    MOZ_ASSERT(!clear);
                    undo_action = DEFER_RESTORE;
                    break;
                  case ActionNode::STORE_POSITION: {
                    Trace::DeferredCapture* pc =
                        static_cast<Trace::DeferredCapture*>(action);
                    if (!clear && store_position == -1) {
                        store_position = pc->cp_offset();
                    }

                    // For captures we know that stores and clears alternate.
                    // Other register, are never cleared, and if the occur
                    // inside a loop, they might be assigned more than once.
                    if (reg <= 1) {
                        // Registers zero and one, aka "capture zero", is
                        // always set correctly if we succeed. There is no
                        // need to undo a setting on backtrack, because we
                        // will set it again or fail.
                        undo_action = DEFER_IGNORE;
                    } else {
                        undo_action = pc->is_capture() ? DEFER_CLEAR : DEFER_RESTORE;
                    }
                    MOZ_ASSERT(!absolute);
                    MOZ_ASSERT(value == 0);
                    break;
                  }
                  case ActionNode::CLEAR_CAPTURES: {
                    // Since we're scanning in reverse order, if we've already
                    // set the position we have to ignore historically earlier
                    // clearing operations.
                    if (store_position == -1) {
                        clear = true;
                    }
                    undo_action = DEFER_RESTORE;
                    MOZ_ASSERT(!absolute);
                    MOZ_ASSERT(value == 0);
                    break;
                  }
                  default:
                    MOZ_CRASH("Bad action");
                }
            }
        }
        // Prepare for the undo-action (e.g., push if it's going to be popped).
        if (undo_action == DEFER_RESTORE) {
            pushes++;
            RegExpMacroAssembler::StackCheckFlag stack_check =
                RegExpMacroAssembler::kNoStackLimitCheck;
            if (pushes == push_limit) {
                stack_check = RegExpMacroAssembler::kCheckStackLimit;
                pushes = 0;
            }

            assembler->PushRegister(reg, stack_check);
            registers_to_pop->Set(alloc, reg);
        } else if (undo_action == DEFER_CLEAR) {
            registers_to_clear->Set(alloc, reg);
        }
        // Perform the chronologically last action (or accumulated increment)
        // for the register.
        if (store_position != -1) {
            assembler->WriteCurrentPositionToRegister(reg, store_position);
        } else if (clear) {
            assembler->ClearRegisters(reg, reg);
        } else if (absolute) {
            assembler->SetRegister(reg, value);
        } else if (value != 0) {
            assembler->AdvanceRegister(reg, value);
        }
    }
}

// This is called as we come into a loop choice node and some other tricky
// nodes.  It normalizes the state of the code generator to ensure we can
// generate generic code.
void Trace::Flush(RegExpCompiler* compiler, RegExpNode* successor)
{
    RegExpMacroAssembler* assembler = compiler->macro_assembler();

    MOZ_ASSERT(!is_trivial());

    if (actions_ == nullptr && backtrack() == nullptr) {
        // Here we just have some deferred cp advances to fix and we are back to
        // a normal situation.  We may also have to forget some information gained
        // through a quick check that was already performed.
        if (cp_offset_ != 0) assembler->AdvanceCurrentPosition(cp_offset_);
        // Create a new trivial state and generate the node with that.
        Trace new_state;
        successor->Emit(compiler, &new_state);
        return;
    }

    // Generate deferred actions here along with code to undo them again.
    OutSet affected_registers;

    if (backtrack() != nullptr) {
        // Here we have a concrete backtrack location.  These are set up by choice
        // nodes and so they indicate that we have a deferred save of the current
        // position which we may need to emit here.
        assembler->PushCurrentPosition();
    }

    int max_register = FindAffectedRegisters(compiler->alloc(), &affected_registers);
    OutSet registers_to_pop;
    OutSet registers_to_clear;
    PerformDeferredActions(compiler->alloc(),
                           assembler,
                           max_register,
                           affected_registers,
                           &registers_to_pop,
                           &registers_to_clear);
    if (cp_offset_ != 0)
        assembler->AdvanceCurrentPosition(cp_offset_);

    // Create a new trivial state and generate the node with that.
    jit::Label undo;
    assembler->PushBacktrack(&undo);
    Trace new_state;
    successor->Emit(compiler, &new_state);

    // On backtrack we need to restore state.
    assembler->BindBacktrack(&undo);
    RestoreAffectedRegisters(assembler,
                             max_register,
                             registers_to_pop,
                             registers_to_clear);
    if (backtrack() == nullptr) {
        assembler->Backtrack();
    } else {
        assembler->PopCurrentPosition();
        assembler->JumpOrBacktrack(backtrack());
    }
}

void
Trace::InvalidateCurrentCharacter()
{
    characters_preloaded_ = 0;
}

void
Trace::AdvanceCurrentPositionInTrace(int by, RegExpCompiler* compiler)
{
    MOZ_ASSERT(by > 0);
    // We don't have an instruction for shifting the current character register
    // down or for using a shifted value for anything so lets just forget that
    // we preloaded any characters into it.
    characters_preloaded_ = 0;
    // Adjust the offsets of the quick check performed information.  This
    // information is used to find out what we already determined about the
    // characters by means of mask and compare.
    quick_check_performed_.Advance(by, compiler->ascii());
    cp_offset_ += by;
    if (cp_offset_ > RegExpMacroAssembler::kMaxCPOffset) {
        compiler->SetRegExpTooBig();
        cp_offset_ = 0;
    }
    bound_checked_up_to_ = Max(0, bound_checked_up_to_ - by);
}

void
OutSet::Set(LifoAlloc* alloc, unsigned value)
{
    if (value < kFirstLimit) {
        first_ |= (1 << value);
    } else {
        if (remaining_ == nullptr)
            remaining_ = alloc->newInfallible<RemainingVector>(*alloc);

        for (size_t i = 0; i < remaining().length(); i++) {
            if (remaining()[i] == value)
                return;
        }
        remaining().append(value);
    }
}

bool
OutSet::Get(unsigned value)
{
    if (value < kFirstLimit)
        return (first_ & (1 << value)) != 0;
    if (remaining_ == nullptr)
        return false;
    for (size_t i = 0; i < remaining().length(); i++) {
        if (remaining()[i] == value)
            return true;
    }
    return false;
}

// -------------------------------------------------------------------
// Graph emitting

void
NegativeSubmatchSuccess::Emit(RegExpCompiler* compiler, Trace* trace)
{
    RegExpMacroAssembler* assembler = compiler->macro_assembler();

    // Omit flushing the trace. We discard the entire stack frame anyway.

    if (!label()->bound()) {
        // We are completely independent of the trace, since we ignore it,
        // so this code can be used as the generic version.
        assembler->Bind(label());
    }

    // Throw away everything on the backtrack stack since the start
    // of the negative submatch and restore the character position.
    assembler->ReadCurrentPositionFromRegister(current_position_register_);
    assembler->ReadBacktrackStackPointerFromRegister(stack_pointer_register_);

    if (clear_capture_count_ > 0) {
        // Clear any captures that might have been performed during the success
        // of the body of the negative look-ahead.
        int clear_capture_end = clear_capture_start_ + clear_capture_count_ - 1;
        assembler->ClearRegisters(clear_capture_start_, clear_capture_end);
    }

    // Now that we have unwound the stack we find at the top of the stack the
    // backtrack that the BeginSubmatch node got.
    assembler->Backtrack();
}

void
EndNode::Emit(RegExpCompiler* compiler, Trace* trace)
{
    if (!trace->is_trivial()) {
        trace->Flush(compiler, this);
        return;
    }
    RegExpMacroAssembler* assembler = compiler->macro_assembler();
    if (!label()->bound()) {
        assembler->Bind(label());
    }
    switch (action_) {
    case ACCEPT:
        assembler->Succeed();
        return;
    case BACKTRACK:
        assembler->JumpOrBacktrack(trace->backtrack());
        return;
    case NEGATIVE_SUBMATCH_SUCCESS:
        // This case is handled in a different virtual method.
        MOZ_CRASH("Bad action: NEGATIVE_SUBMATCH_SUCCESS");
    }
    MOZ_CRASH("Bad action");
}

// Emit the code to check for a ^ in multiline mode (1-character lookbehind
// that matches newline or the start of input).
static void
EmitHat(RegExpCompiler* compiler, RegExpNode* on_success, Trace* trace)
{
    RegExpMacroAssembler* assembler = compiler->macro_assembler();

    // We will be loading the previous character into the current character
    // register.
    Trace new_trace(*trace);
    new_trace.InvalidateCurrentCharacter();

    jit::Label ok;
    if (new_trace.cp_offset() == 0) {
        // The start of input counts as a newline in this context, so skip to
        // ok if we are at the start.
        assembler->CheckAtStart(&ok);
    }

    // We already checked that we are not at the start of input so it must be
    // OK to load the previous character.
    assembler->LoadCurrentCharacter(new_trace.cp_offset() -1, new_trace.backtrack(), false);

    if (!assembler->CheckSpecialCharacterClass('n', new_trace.backtrack())) {
        // Newline means \n, \r, 0x2028 or 0x2029.
        if (!compiler->ascii())
            assembler->CheckCharacterAfterAnd(0x2028, 0xfffe, &ok);
        assembler->CheckCharacter('\n', &ok);
        assembler->CheckNotCharacter('\r', new_trace.backtrack());
    }
    assembler->Bind(&ok);
    on_success->Emit(compiler, &new_trace);
}

// Assert that the next character cannot be a part of a surrogate pair.
static void
EmitNotAfterLeadSurrogate(RegExpCompiler* compiler, RegExpNode* on_success, Trace* trace)
{
    RegExpMacroAssembler* assembler = compiler->macro_assembler();

    // We will be loading the previous character into the current character
    // register.
    Trace new_trace(*trace);
    new_trace.InvalidateCurrentCharacter();

    jit::Label ok;
    if (new_trace.cp_offset() == 0)
        assembler->CheckAtStart(&ok);

    // We already checked that we are not at the start of input so it must be
    // OK to load the previous character.
    assembler->LoadCurrentCharacter(new_trace.cp_offset() - 1, new_trace.backtrack(), false);
    assembler->CheckCharacterInRange(unicode::LeadSurrogateMin, unicode::LeadSurrogateMax,
                                     new_trace.backtrack());

    assembler->Bind(&ok);
    on_success->Emit(compiler, &new_trace);
}

// Assert that the next character is not a trail surrogate that has a
// corresponding lead surrogate.
static void
EmitNotInSurrogatePair(RegExpCompiler* compiler, RegExpNode* on_success, Trace* trace)
{
    RegExpMacroAssembler* assembler = compiler->macro_assembler();

    jit::Label ok;
    assembler->CheckPosition(trace->cp_offset(), &ok);

    // We will be loading the next and previous characters into the current
    // character register.
    Trace new_trace(*trace);
    new_trace.InvalidateCurrentCharacter();

    if (new_trace.cp_offset() == 0)
        assembler->CheckAtStart(&ok);

    // First check if next character is a trail surrogate.
    assembler->LoadCurrentCharacter(new_trace.cp_offset(), new_trace.backtrack(), false);
    assembler->CheckCharacterNotInRange(unicode::TrailSurrogateMin, unicode::TrailSurrogateMax,
                                        &ok);

    // Next check if previous character is a lead surrogate.
    // We already checked that we are not at the start of input so it must be
    // OK to load the previous character.
    assembler->LoadCurrentCharacter(new_trace.cp_offset() - 1, new_trace.backtrack(), false);
    assembler->CheckCharacterInRange(unicode::LeadSurrogateMin, unicode::LeadSurrogateMax,
                                     new_trace.backtrack());

    assembler->Bind(&ok);
    on_success->Emit(compiler, &new_trace);
}

// Check for [0-9A-Z_a-z].
static void
EmitWordCheck(RegExpMacroAssembler* assembler,
              jit::Label* word, jit::Label* non_word, bool fall_through_on_word,
              bool unicode_ignore_case)
{
    if (!unicode_ignore_case &&
        assembler->CheckSpecialCharacterClass(fall_through_on_word ? 'w' : 'W',
                                              fall_through_on_word ? non_word : word))
    {
        // Optimized implementation available.
        return;
    }

    if (unicode_ignore_case) {
        assembler->CheckCharacter(0x017F, word);
        assembler->CheckCharacter(0x212A, word);
    }

    assembler->CheckCharacterGT('z', non_word);
    assembler->CheckCharacterLT('0', non_word);
    assembler->CheckCharacterGT('a' - 1, word);
    assembler->CheckCharacterLT('9' + 1, word);
    assembler->CheckCharacterLT('A', non_word);
    assembler->CheckCharacterLT('Z' + 1, word);

    if (fall_through_on_word)
        assembler->CheckNotCharacter('_', non_word);
    else
        assembler->CheckCharacter('_', word);
}

// Emit the code to handle \b and \B (word-boundary or non-word-boundary).
void
AssertionNode::EmitBoundaryCheck(RegExpCompiler* compiler, Trace* trace)
{
    RegExpMacroAssembler* assembler = compiler->macro_assembler();
    Trace::TriBool next_is_word_character = Trace::UNKNOWN;
    bool not_at_start = (trace->at_start() == Trace::FALSE_VALUE);
    BoyerMooreLookahead* lookahead = bm_info(not_at_start);
    if (lookahead == nullptr) {
        int eats_at_least =
            Min(kMaxLookaheadForBoyerMoore, EatsAtLeast(kMaxLookaheadForBoyerMoore,
                                                        kRecursionBudget,
                                                        not_at_start));
        if (eats_at_least >= 1) {
            BoyerMooreLookahead* bm =
                alloc()->newInfallible<BoyerMooreLookahead>(alloc(), eats_at_least, compiler);
            FillInBMInfo(0, kRecursionBudget, bm, not_at_start);
            if (bm->at(0)->is_non_word())
                next_is_word_character = Trace::FALSE_VALUE;
            if (bm->at(0)->is_word()) next_is_word_character = Trace::TRUE_VALUE;
        }
    } else {
        if (lookahead->at(0)->is_non_word())
            next_is_word_character = Trace::FALSE_VALUE;
        if (lookahead->at(0)->is_word())
            next_is_word_character = Trace::TRUE_VALUE;
    }
    bool at_boundary = (assertion_type_ == AssertionNode::AT_BOUNDARY);
    if (next_is_word_character == Trace::UNKNOWN) {
        jit::Label before_non_word;
        jit::Label before_word;
        if (trace->characters_preloaded() != 1) {
            assembler->LoadCurrentCharacter(trace->cp_offset(), &before_non_word);
        }
        // Fall through on non-word.
        EmitWordCheck(assembler, &before_word, &before_non_word, false,
                      compiler->unicode() && compiler->ignore_case());
        // Next character is not a word character.
        assembler->Bind(&before_non_word);
        jit::Label ok;
        BacktrackIfPrevious(compiler, trace, at_boundary ? kIsNonWord : kIsWord);
        assembler->JumpOrBacktrack(&ok);

        assembler->Bind(&before_word);
        BacktrackIfPrevious(compiler, trace, at_boundary ? kIsWord : kIsNonWord);
        assembler->Bind(&ok);
    } else if (next_is_word_character == Trace::TRUE_VALUE) {
        BacktrackIfPrevious(compiler, trace, at_boundary ? kIsWord : kIsNonWord);
    } else {
        MOZ_ASSERT(next_is_word_character == Trace::FALSE_VALUE);
        BacktrackIfPrevious(compiler, trace, at_boundary ? kIsNonWord : kIsWord);
    }
}

void
AssertionNode::BacktrackIfPrevious(RegExpCompiler* compiler,
                                   Trace* trace,
                                   AssertionNode::IfPrevious backtrack_if_previous)
{
    RegExpMacroAssembler* assembler = compiler->macro_assembler();
    Trace new_trace(*trace);
    new_trace.InvalidateCurrentCharacter();

    jit::Label fall_through, dummy;

    jit::Label* non_word = backtrack_if_previous == kIsNonWord ? new_trace.backtrack() : &fall_through;
    jit::Label* word     = backtrack_if_previous == kIsNonWord ? &fall_through : new_trace.backtrack();

    if (new_trace.cp_offset() == 0) {
        // The start of input counts as a non-word character, so the question is
        // decided if we are at the start.
        assembler->CheckAtStart(non_word);
    }
    // We already checked that we are not at the start of input so it must be
    // OK to load the previous character.
    assembler->LoadCurrentCharacter(new_trace.cp_offset() - 1, &dummy, false);
    EmitWordCheck(assembler, word, non_word, backtrack_if_previous == kIsNonWord,
                  compiler->unicode() && compiler->ignore_case());

    assembler->Bind(&fall_through);
    on_success()->Emit(compiler, &new_trace);
}

void
AssertionNode::GetQuickCheckDetails(QuickCheckDetails* details,
                                    RegExpCompiler* compiler,
                                    int filled_in,
                                    bool not_at_start)
{
    if (assertion_type_ == AT_START && not_at_start) {
        details->set_cannot_match();
        return;
    }
    return on_success()->GetQuickCheckDetails(details, compiler, filled_in, not_at_start);
}

void
AssertionNode::Emit(RegExpCompiler* compiler, Trace* trace)
{
    RegExpMacroAssembler* assembler = compiler->macro_assembler();
    switch (assertion_type_) {
      case AT_END: {
        jit::Label ok;
        assembler->CheckPosition(trace->cp_offset(), &ok);
        assembler->JumpOrBacktrack(trace->backtrack());
        assembler->Bind(&ok);
        break;
      }
      case AT_START: {
        if (trace->at_start() == Trace::FALSE_VALUE) {
            assembler->JumpOrBacktrack(trace->backtrack());
            return;
        }
        if (trace->at_start() == Trace::UNKNOWN) {
            assembler->CheckNotAtStart(trace->backtrack());
            Trace at_start_trace = *trace;
            at_start_trace.set_at_start(true);
            on_success()->Emit(compiler, &at_start_trace);
            return;
        }
      }
        break;
      case AFTER_NEWLINE:
        EmitHat(compiler, on_success(), trace);
        return;
      case AT_BOUNDARY:
      case AT_NON_BOUNDARY: {
        EmitBoundaryCheck(compiler, trace);
        return;
      }
      case NOT_AFTER_LEAD_SURROGATE:
        EmitNotAfterLeadSurrogate(compiler, on_success(), trace);
        return;
      case NOT_IN_SURROGATE_PAIR:
        EmitNotInSurrogatePair(compiler, on_success(), trace);
        return;
    }
    on_success()->Emit(compiler, trace);
}

static bool
DeterminedAlready(QuickCheckDetails* quick_check, int offset)
{
    if (quick_check == nullptr)
        return false;
    if (offset >= quick_check->characters())
        return false;
    return quick_check->positions(offset)->determines_perfectly;
}

static void
UpdateBoundsCheck(int index, int* checked_up_to)
{
    if (index > *checked_up_to)
        *checked_up_to = index;
}

static void
EmitBoundaryTest(RegExpMacroAssembler* masm,
                 int border,
                 jit::Label* fall_through,
                 jit::Label* above_or_equal,
                 jit::Label* below)
{
    if (below != fall_through) {
        masm->CheckCharacterLT(border, below);
        if (above_or_equal != fall_through)
            masm->JumpOrBacktrack(above_or_equal);
    } else {
        masm->CheckCharacterGT(border - 1, above_or_equal);
    }
}

static void
EmitDoubleBoundaryTest(RegExpMacroAssembler* masm,
                       int first,
                       int last,
                       jit::Label* fall_through,
                       jit::Label* in_range,
                       jit::Label* out_of_range)
{
    if (in_range == fall_through) {
        if (first == last)
            masm->CheckNotCharacter(first, out_of_range);
        else
            masm->CheckCharacterNotInRange(first, last, out_of_range);
    } else {
        if (first == last)
            masm->CheckCharacter(first, in_range);
        else
            masm->CheckCharacterInRange(first, last, in_range);
        if (out_of_range != fall_through)
            masm->JumpOrBacktrack(out_of_range);
    }
}

typedef InfallibleVector<int, 4> RangeBoundaryVector;

// even_label is for ranges[i] to ranges[i + 1] where i - start_index is even.
// odd_label is for ranges[i] to ranges[i + 1] where i - start_index is odd.
static void
EmitUseLookupTable(RegExpMacroAssembler* masm,
                   RangeBoundaryVector& ranges,
                   int start_index,
                   int end_index,
                   int min_char,
                   jit::Label* fall_through,
                   jit::Label* even_label,
                   jit::Label* odd_label)
{
    static const int kSize = RegExpMacroAssembler::kTableSize;
    static const int kMask = RegExpMacroAssembler::kTableMask;

    DebugOnly<int> base = (min_char & ~kMask);

    // Assert that everything is on one kTableSize page.
    for (int i = start_index; i <= end_index; i++)
        MOZ_ASSERT((ranges[i] & ~kMask) == base);
    MOZ_ASSERT(start_index == 0 || (ranges[start_index - 1] & ~kMask) <= base);

    char templ[kSize];
    jit::Label* on_bit_set;
    jit::Label* on_bit_clear;
    int bit;
    if (even_label == fall_through) {
        on_bit_set = odd_label;
        on_bit_clear = even_label;
        bit = 1;
    } else {
        on_bit_set = even_label;
        on_bit_clear = odd_label;
        bit = 0;
    }
    for (int i = 0; i < (ranges[start_index] & kMask) && i < kSize; i++)
        templ[i] = bit;
    int j = 0;
    bit ^= 1;
    for (int i = start_index; i < end_index; i++) {
        for (j = (ranges[i] & kMask); j < (ranges[i + 1] & kMask); j++) {
            templ[j] = bit;
        }
        bit ^= 1;
    }
    for (int i = j; i < kSize; i++) {
        templ[i] = bit;
    }

    // TODO(erikcorry): Cache these.
    uint8_t* ba;
    {
        AutoEnterOOMUnsafeRegion oomUnsafe;
        ba = static_cast<uint8_t*>(js_malloc(kSize));
        if (!ba || !masm->shared->addTable(ba))
            oomUnsafe.crash("Table malloc");
    }

    for (int i = 0; i < kSize; i++)
        ba[i] = templ[i];

    masm->CheckBitInTable(ba, on_bit_set);
    if (on_bit_clear != fall_through)
        masm->JumpOrBacktrack(on_bit_clear);
}

static void
CutOutRange(RegExpMacroAssembler* masm,
            RangeBoundaryVector& ranges,
            int start_index,
            int end_index,
            int cut_index,
            jit::Label* even_label,
            jit::Label* odd_label)
{
    bool odd = (((cut_index - start_index) & 1) == 1);
    jit::Label* in_range_label = odd ? odd_label : even_label;
    jit::Label dummy;
    EmitDoubleBoundaryTest(masm,
                           ranges[cut_index],
                           ranges[cut_index + 1] - 1,
                           &dummy,
                           in_range_label,
                           &dummy);
    MOZ_ASSERT(!dummy.used());
    // Cut out the single range by rewriting the array.  This creates a new
    // range that is a merger of the two ranges on either side of the one we
    // are cutting out.  The oddity of the labels is preserved.
    for (int j = cut_index; j > start_index; j--)
        ranges[j] = ranges[j - 1];
    for (int j = cut_index + 1; j < end_index; j++)
        ranges[j] = ranges[j + 1];
}

// Unicode case.  Split the search space into kSize spaces that are handled
// with recursion.
static void
SplitSearchSpace(RangeBoundaryVector& ranges,
                 int start_index,
                 int end_index,
                 int* new_start_index,
                 int* new_end_index,
                 int* border)
{
    static const int kSize = RegExpMacroAssembler::kTableSize;
    static const int kMask = RegExpMacroAssembler::kTableMask;

    int first = ranges[start_index];
    int last = ranges[end_index] - 1;

    *new_start_index = start_index;
    *border = (ranges[start_index] & ~kMask) + kSize;
    while (*new_start_index < end_index) {
        if (ranges[*new_start_index] > *border)
            break;
        (*new_start_index)++;
    }
    // new_start_index is the index of the first edge that is beyond the
    // current kSize space.

    // For very large search spaces we do a binary chop search of the non-ASCII
    // space instead of just going to the end of the current kSize space.  The
    // heuristics are complicated a little by the fact that any 128-character
    // encoding space can be quickly tested with a table lookup, so we don't
    // wish to do binary chop search at a smaller granularity than that.  A
    // 128-character space can take up a lot of space in the ranges array if,
    // for example, we only want to match every second character (eg. the lower
    // case characters on some Unicode pages).
    int binary_chop_index = (end_index + start_index) / 2;
    // The first test ensures that we get to the code that handles the ASCII
    // range with a single not-taken branch, speeding up this important
    // character range (even non-ASCII charset-based text has spaces and
    // punctuation).
    if (*border - 1 > kMaxOneByteCharCode &&  // ASCII case.
        end_index - start_index > (*new_start_index - start_index) * 2 &&
        last - first > kSize * 2 &&
        binary_chop_index > *new_start_index &&
        ranges[binary_chop_index] >= first + 2 * kSize)
    {
        int scan_forward_for_section_border = binary_chop_index;;
        int new_border = (ranges[binary_chop_index] | kMask) + 1;

        while (scan_forward_for_section_border < end_index) {
            if (ranges[scan_forward_for_section_border] > new_border) {
                *new_start_index = scan_forward_for_section_border;
                *border = new_border;
                break;
            }
            scan_forward_for_section_border++;
        }
    }

    MOZ_ASSERT(*new_start_index > start_index);
    *new_end_index = *new_start_index - 1;
    if (ranges[*new_end_index] == *border)
        (*new_end_index)--;
    if (*border >= ranges[end_index]) {
        *border = ranges[end_index];
        *new_start_index = end_index;  // Won't be used.
        *new_end_index = end_index - 1;
    }
}

// Gets a series of segment boundaries representing a character class.  If the
// character is in the range between an even and an odd boundary (counting from
// start_index) then go to even_label, otherwise go to odd_label.  We already
// know that the character is in the range of min_char to max_char inclusive.
// Either label can be nullptr indicating backtracking.  Either label can also be
// equal to the fall_through label.
static void
GenerateBranches(RegExpMacroAssembler* masm,
                 RangeBoundaryVector& ranges,
                 int start_index,
                 int end_index,
                 char16_t min_char,
                 char16_t max_char,
                 jit::Label* fall_through,
                 jit::Label* even_label,
                 jit::Label* odd_label)
{
    int first = ranges[start_index];
    int last = ranges[end_index] - 1;

    MOZ_ASSERT(min_char < first);

    // Just need to test if the character is before or on-or-after
    // a particular character.
    if (start_index == end_index) {
        EmitBoundaryTest(masm, first, fall_through, even_label, odd_label);
        return;
    }

    // Another almost trivial case:  There is one interval in the middle that is
    // different from the end intervals.
    if (start_index + 1 == end_index) {
        EmitDoubleBoundaryTest(masm, first, last, fall_through, even_label, odd_label);
        return;
    }

    // It's not worth using table lookup if there are very few intervals in the
    // character class.
    if (end_index - start_index <= 6) {
        // It is faster to test for individual characters, so we look for those
        // first, then try arbitrary ranges in the second round.
        static int kNoCutIndex = -1;
        int cut = kNoCutIndex;
        for (int i = start_index; i < end_index; i++) {
            if (ranges[i] == ranges[i + 1] - 1) {
                cut = i;
                break;
            }
        }
        if (cut == kNoCutIndex) cut = start_index;
        CutOutRange(masm, ranges, start_index, end_index, cut, even_label, odd_label);
        MOZ_ASSERT(end_index - start_index >= 2);
        GenerateBranches(masm,
                         ranges,
                         start_index + 1,
                         end_index - 1,
                         min_char,
                         max_char,
                         fall_through,
                         even_label,
                         odd_label);
        return;
    }

    // If there are a lot of intervals in the regexp, then we will use tables to
    // determine whether the character is inside or outside the character class.
    static const int kBits = RegExpMacroAssembler::kTableSizeBits;

    if ((max_char >> kBits) == (min_char >> kBits)) {
        EmitUseLookupTable(masm,
                           ranges,
                           start_index,
                           end_index,
                           min_char,
                           fall_through,
                           even_label,
                           odd_label);
        return;
    }

    if ((min_char >> kBits) != (first >> kBits)) {
        masm->CheckCharacterLT(first, odd_label);
        GenerateBranches(masm,
                         ranges,
                         start_index + 1,
                         end_index,
                         first,
                         max_char,
                         fall_through,
                         odd_label,
                         even_label);
        return;
    }

    int new_start_index = 0;
    int new_end_index = 0;
    int border = 0;

    SplitSearchSpace(ranges,
                     start_index,
                     end_index,
                     &new_start_index,
                     &new_end_index,
                     &border);

    jit::Label handle_rest;
    jit::Label* above = &handle_rest;
    if (border == last + 1) {
        // We didn't find any section that started after the limit, so everything
        // above the border is one of the terminal labels.
        above = (end_index & 1) != (start_index & 1) ? odd_label : even_label;
        MOZ_ASSERT(new_end_index == end_index - 1);
    }

    MOZ_ASSERT(start_index <= new_end_index);
    MOZ_ASSERT(new_start_index <= end_index);
    MOZ_ASSERT(start_index < new_start_index);
    MOZ_ASSERT(new_end_index < end_index);
    MOZ_ASSERT(new_end_index + 1 == new_start_index ||
               (new_end_index + 2 == new_start_index &&
                border == ranges[new_end_index + 1]));
    MOZ_ASSERT(min_char < border - 1);
    MOZ_ASSERT(border < max_char);
    MOZ_ASSERT(ranges[new_end_index] < border);
    MOZ_ASSERT(border < ranges[new_start_index] ||
               (border == ranges[new_start_index] &&
                new_start_index == end_index &&
                new_end_index == end_index - 1 &&
                border == last + 1));
    MOZ_ASSERT(new_start_index == 0 || border >= ranges[new_start_index - 1]);

    masm->CheckCharacterGT(border - 1, above);
    jit::Label dummy;
    GenerateBranches(masm,
                     ranges,
                     start_index,
                     new_end_index,
                     min_char,
                     border - 1,
                     &dummy,
                     even_label,
                     odd_label);
    if (handle_rest.used()) {
        masm->Bind(&handle_rest);
        bool flip = (new_start_index & 1) != (start_index & 1);
        GenerateBranches(masm,
                         ranges,
                         new_start_index,
                         end_index,
                         border,
                         max_char,
                         &dummy,
                         flip ? odd_label : even_label,
                         flip ? even_label : odd_label);
    }
}

static void
EmitCharClass(LifoAlloc* alloc,
              RegExpMacroAssembler* macro_assembler,
              RegExpCharacterClass* cc,
              bool ascii,
              jit::Label* on_failure,
              int cp_offset,
              bool check_offset,
              bool preloaded)
{
    CharacterRangeVector& ranges = cc->ranges(alloc);
    if (!CharacterRange::IsCanonical(ranges)) {
        CharacterRange::Canonicalize(ranges);
    }

    int max_char = MaximumCharacter(ascii);
    int range_count = ranges.length();

    int last_valid_range = range_count - 1;
    while (last_valid_range >= 0) {
        CharacterRange& range = ranges[last_valid_range];
        if (range.from() <= max_char) {
            break;
        }
        last_valid_range--;
    }

    if (last_valid_range < 0) {
        if (!cc->is_negated()) {
            macro_assembler->JumpOrBacktrack(on_failure);
        }
        if (check_offset) {
            macro_assembler->CheckPosition(cp_offset, on_failure);
        }
        return;
    }

    if (last_valid_range == 0 &&
        ranges[0].IsEverything(max_char)) {
        if (cc->is_negated()) {
            macro_assembler->JumpOrBacktrack(on_failure);
        } else {
            // This is a common case hit by non-anchored expressions.
            if (check_offset) {
                macro_assembler->CheckPosition(cp_offset, on_failure);
            }
        }
        return;
    }
    if (last_valid_range == 0 &&
        !cc->is_negated() &&
        ranges[0].IsEverything(max_char)) {
        // This is a common case hit by non-anchored expressions.
        if (check_offset) {
            macro_assembler->CheckPosition(cp_offset, on_failure);
        }
        return;
    }

    if (!preloaded) {
        macro_assembler->LoadCurrentCharacter(cp_offset, on_failure, check_offset);
    }

    if (cc->is_standard(alloc) &&
        macro_assembler->CheckSpecialCharacterClass(cc->standard_type(),
                                                    on_failure)) {
        return;
    }

    // A new list with ascending entries.  Each entry is a code unit
    // where there is a boundary between code units that are part of
    // the class and code units that are not.  Normally we insert an
    // entry at zero which goes to the failure label, but if there
    // was already one there we fall through for success on that entry.
    // Subsequent entries have alternating meaning (success/failure).
    RangeBoundaryVector* range_boundaries =
        alloc->newInfallible<RangeBoundaryVector>(*alloc);

    bool zeroth_entry_is_failure = !cc->is_negated();

    range_boundaries->reserve(last_valid_range);
    for (int i = 0; i <= last_valid_range; i++) {
        CharacterRange& range = ranges[i];
        if (range.from() == 0) {
            MOZ_ASSERT(i == 0);
            zeroth_entry_is_failure = !zeroth_entry_is_failure;
        } else {
            range_boundaries->append(range.from());
        }
        range_boundaries->append(range.to() + 1);
    }
    int end_index = range_boundaries->length() - 1;
    if ((*range_boundaries)[end_index] > max_char)
        end_index--;

    jit::Label fall_through;
    GenerateBranches(macro_assembler,
                     *range_boundaries,
                     0,  // start_index.
                     end_index,
                     0,  // min_char.
                     max_char,
                     &fall_through,
                     zeroth_entry_is_failure ? &fall_through : on_failure,
                     zeroth_entry_is_failure ? on_failure : &fall_through);
    macro_assembler->Bind(&fall_through);
}

typedef bool EmitCharacterFunction(RegExpCompiler* compiler,
                                   char16_t c,
                                   jit::Label* on_failure,
                                   int cp_offset,
                                   bool check,
                                   bool preloaded);

static inline bool
EmitSimpleCharacter(RegExpCompiler* compiler,
                    char16_t c,
                    jit::Label* on_failure,
                    int cp_offset,
                    bool check,
                    bool preloaded)
{
    RegExpMacroAssembler* assembler = compiler->macro_assembler();
    bool bound_checked = false;
    if (!preloaded) {
        assembler->LoadCurrentCharacter(cp_offset, on_failure, check);
        bound_checked = true;
    }
    assembler->CheckNotCharacter(c, on_failure);
    return bound_checked;
}

// Emit character for case independent match, when GetCaseIndependentLetters
// returns single character.
// This is used by the following 2 cases:
//   * non-letters (things that don't have case)
//   * letters that map across Latin1 and non-Latin1, and non-Latin1 case is
//     filtered out because of Latin1 match
static inline bool
EmitAtomSingle(RegExpCompiler* compiler,
               char16_t c,
               jit::Label* on_failure,
               int cp_offset,
               bool check,
               bool preloaded)
{
    RegExpMacroAssembler* macro_assembler = compiler->macro_assembler();
    // FIXME: `ascii` actually means latin1 (bug 1338841).
    bool ascii = compiler->ascii();
    char16_t chars[kEcma262UnCanonicalizeMaxWidth];
    int length = GetCaseIndependentLetters(c, ascii, compiler->unicode(), chars);
    if (length != 1)
        return false;

    bool checked = false;
    if (!preloaded) {
        macro_assembler->LoadCurrentCharacter(cp_offset, on_failure, check);
        checked = check;
    }
    macro_assembler->CheckNotCharacter(chars[0], on_failure);
    return checked;
}

static bool
ShortCutEmitCharacterPair(RegExpMacroAssembler* macro_assembler,
                          bool ascii,
                          char16_t c1,
                          char16_t c2,
                          jit::Label* on_failure)
{
    char16_t char_mask = MaximumCharacter(ascii);

    MOZ_ASSERT(c1 != c2);
    if (c1 > c2) {
        char16_t tmp = c1;
        c1 = c2;
        c2 = tmp;
    }

    char16_t exor = c1 ^ c2;
    // Check whether exor has only one bit set.
    if (((exor - 1) & exor) == 0) {
        // If c1 and c2 differ only by one bit.
        char16_t mask = char_mask ^ exor;
        macro_assembler->CheckNotCharacterAfterAnd(c1, mask, on_failure);
        return true;
    }

    char16_t diff = c2 - c1;
    if (((diff - 1) & diff) == 0 && c1 >= diff) {
        // If the characters differ by 2^n but don't differ by one bit then
        // subtract the difference from the found character, then do the or
        // trick.  We avoid the theoretical case where negative numbers are
        // involved in order to simplify code generation.
        char16_t mask = char_mask ^ diff;
        macro_assembler->CheckNotCharacterAfterMinusAnd(c1 - diff,
                                                        diff,
                                                        mask,
                                                        on_failure);
        return true;
    }
    return false;
}

// Emit character for case independent match, when GetCaseIndependentLetters
// returns multiple characters.
static inline bool
EmitAtomMulti(RegExpCompiler* compiler,
              char16_t c,
              jit::Label* on_failure,
              int cp_offset,
              bool check,
              bool preloaded)
{
    RegExpMacroAssembler* macro_assembler = compiler->macro_assembler();
    bool ascii = compiler->ascii();
    char16_t chars[kEcma262UnCanonicalizeMaxWidth];
    int length = GetCaseIndependentLetters(c, ascii, compiler->unicode(), chars);
    if (length <= 1) return false;
    // We may not need to check against the end of the input string
    // if this character lies before a character that matched.
    if (!preloaded)
        macro_assembler->LoadCurrentCharacter(cp_offset, on_failure, check);
    jit::Label ok;
    MOZ_ASSERT(kEcma262UnCanonicalizeMaxWidth == 4);
    switch (length) {
      case 2: {
        if (ShortCutEmitCharacterPair(macro_assembler,
                                      ascii,
                                      chars[0],
                                      chars[1],
                                      on_failure)) {
        } else {
            macro_assembler->CheckCharacter(chars[0], &ok);
            macro_assembler->CheckNotCharacter(chars[1], on_failure);
            macro_assembler->Bind(&ok);
        }
        break;
      }
      case 4:
        macro_assembler->CheckCharacter(chars[3], &ok);
        MOZ_FALLTHROUGH;
      case 3:
        macro_assembler->CheckCharacter(chars[0], &ok);
        macro_assembler->CheckCharacter(chars[1], &ok);
        macro_assembler->CheckNotCharacter(chars[2], on_failure);
        macro_assembler->Bind(&ok);
        break;
      default:
        MOZ_CRASH("Bad length");
    }
    return true;
}

// We call this repeatedly to generate code for each pass over the text node.
// The passes are in increasing order of difficulty because we hope one
// of the first passes will fail in which case we are saved the work of the
// later passes.  for example for the case independent regexp /%[asdfghjkl]a/
// we will check the '%' in the first pass, the case independent 'a' in the
// second pass and the character class in the last pass.
//
// The passes are done from right to left, so for example to test for /bar/
// we will first test for an 'r' with offset 2, then an 'a' with offset 1
// and then a 'b' with offset 0.  This means we can avoid the end-of-input
// bounds check most of the time.  In the example we only need to check for
// end-of-input when loading the putative 'r'.
//
// A slight complication involves the fact that the first character may already
// be fetched into a register by the previous node.  In this case we want to
// do the test for that character first.  We do this in separate passes.  The
// 'preloaded' argument indicates that we are doing such a 'pass'.  If such a
// pass has been performed then subsequent passes will have true in
// first_element_checked to indicate that that character does not need to be
// checked again.
//
// In addition to all this we are passed a Trace, which can
// contain an AlternativeGeneration object.  In this AlternativeGeneration
// object we can see details of any quick check that was already passed in
// order to get to the code we are now generating.  The quick check can involve
// loading characters, which means we do not need to recheck the bounds
// up to the limit the quick check already checked.  In addition the quick
// check can have involved a mask and compare operation which may simplify
// or obviate the need for further checks at some character positions.
void
TextNode::TextEmitPass(RegExpCompiler* compiler,
                       TextEmitPassType pass,
                       bool preloaded,
                       Trace* trace,
                       bool first_element_checked,
                       int* checked_up_to)
{
    RegExpMacroAssembler* assembler = compiler->macro_assembler();
    bool ascii = compiler->ascii();
    jit::Label* backtrack = trace->backtrack();
    QuickCheckDetails* quick_check = trace->quick_check_performed();
    int element_count = elements().length();
    for (int i = preloaded ? 0 : element_count - 1; i >= 0; i--) {
        TextElement elm = elements()[i];
        int cp_offset = trace->cp_offset() + elm.cp_offset();
        if (elm.text_type() == TextElement::ATOM) {
            const CharacterVector& quarks = elm.atom()->data();
            for (int j = preloaded ? 0 : quarks.length() - 1; j >= 0; j--) {
                if (first_element_checked && i == 0 && j == 0) continue;
                if (DeterminedAlready(quick_check, elm.cp_offset() + j)) continue;
                EmitCharacterFunction* emit_function = nullptr;
                switch (pass) {
                  case NON_ASCII_MATCH:
                    MOZ_ASSERT(ascii);
                    if (!IsLatin1Equivalent(quarks[j], compiler)) {
                        assembler->JumpOrBacktrack(backtrack);
                        return;
                    }
                    break;
                  case CASE_SINGLE_CHARACTER_MATCH:
                    emit_function = &EmitAtomSingle;
                    break;
                  case SIMPLE_CHARACTER_MATCH:
                    emit_function = &EmitSimpleCharacter;
                    break;
                  case CASE_MUTLI_CHARACTER_MATCH:
                    emit_function = &EmitAtomMulti;
                    break;
                  default:
                    break;
                }
                if (emit_function != nullptr) {
                    bool bound_checked = emit_function(compiler,
                                                       quarks[j],
                                                       backtrack,
                                                       cp_offset + j,
                                                       *checked_up_to < cp_offset + j,
                                                       preloaded);
                    if (bound_checked) UpdateBoundsCheck(cp_offset + j, checked_up_to);
                }
            }
        } else {
            MOZ_ASSERT(TextElement::CHAR_CLASS == elm.text_type());
            if (pass == CHARACTER_CLASS_MATCH) {
                if (first_element_checked && i == 0) continue;
                if (DeterminedAlready(quick_check, elm.cp_offset())) continue;
                RegExpCharacterClass* cc = elm.char_class();
                EmitCharClass(alloc(),
                              assembler,
                              cc,
                              ascii,
                              backtrack,
                              cp_offset,
                              *checked_up_to < cp_offset,
                              preloaded);
                UpdateBoundsCheck(cp_offset, checked_up_to);
            }
        }
    }
}

int
TextNode::Length()
{
    TextElement elm = elements()[elements().length() - 1];
    MOZ_ASSERT(elm.cp_offset() >= 0);
    return elm.cp_offset() + elm.length();
}

bool
TextNode::SkipPass(int int_pass, bool ignore_case)
{
    TextEmitPassType pass = static_cast<TextEmitPassType>(int_pass);
    if (ignore_case)
        return pass == SIMPLE_CHARACTER_MATCH;
    return pass == CASE_SINGLE_CHARACTER_MATCH || pass == CASE_MUTLI_CHARACTER_MATCH;
}

// This generates the code to match a text node.  A text node can contain
// straight character sequences (possibly to be matched in a case-independent
// way) and character classes.  For efficiency we do not do this in a single
// pass from left to right.  Instead we pass over the text node several times,
// emitting code for some character positions every time.  See the comment on
// TextEmitPass for details.
void
TextNode::Emit(RegExpCompiler* compiler, Trace* trace)
{
    LimitResult limit_result = LimitVersions(compiler, trace);
    if (limit_result == DONE) return;
    MOZ_ASSERT(limit_result == CONTINUE);

    if (trace->cp_offset() + Length() > RegExpMacroAssembler::kMaxCPOffset) {
        compiler->SetRegExpTooBig();
        return;
    }

    if (compiler->ascii()) {
        int dummy = 0;
        TextEmitPass(compiler, NON_ASCII_MATCH, false, trace, false, &dummy);
    }

    bool first_elt_done = false;
    int bound_checked_to = trace->cp_offset() - 1;
    bound_checked_to += trace->bound_checked_up_to();

    // If a character is preloaded into the current character register then
    // check that now.
    if (trace->characters_preloaded() == 1) {
        for (int pass = kFirstRealPass; pass <= kLastPass; pass++) {
            if (!SkipPass(pass, compiler->ignore_case())) {
                TextEmitPass(compiler,
                             static_cast<TextEmitPassType>(pass),
                             true,
                             trace,
                             false,
                             &bound_checked_to);
            }
        }
        first_elt_done = true;
    }

    for (int pass = kFirstRealPass; pass <= kLastPass; pass++) {
        if (!SkipPass(pass, compiler->ignore_case())) {
            TextEmitPass(compiler,
                         static_cast<TextEmitPassType>(pass),
                         false,
                         trace,
                         first_elt_done,
                         &bound_checked_to);
        }
    }

    Trace successor_trace(*trace);
    successor_trace.set_at_start(false);
    successor_trace.AdvanceCurrentPositionInTrace(Length(), compiler);
    RecursionCheck rc(compiler);
    on_success()->Emit(compiler, &successor_trace);
}

void
LoopChoiceNode::Emit(RegExpCompiler* compiler, Trace* trace)
{
    RegExpMacroAssembler* macro_assembler = compiler->macro_assembler();
    if (trace->stop_node() == this) {
        int text_length =
            GreedyLoopTextLengthForAlternative(&alternatives()[0]);
        MOZ_ASSERT(text_length != kNodeIsTooComplexForGreedyLoops);
        // Update the counter-based backtracking info on the stack.  This is an
        // optimization for greedy loops (see below).
        MOZ_ASSERT(trace->cp_offset() == text_length);
        macro_assembler->AdvanceCurrentPosition(text_length);
        macro_assembler->JumpOrBacktrack(trace->loop_label());
        return;
    }
    MOZ_ASSERT(trace->stop_node() == nullptr);
    if (!trace->is_trivial()) {
        trace->Flush(compiler, this);
        return;
    }
    ChoiceNode::Emit(compiler, trace);
}

/* Code generation for choice nodes.
 *
 * We generate quick checks that do a mask and compare to eliminate a
 * choice.  If the quick check succeeds then it jumps to the continuation to
 * do slow checks and check subsequent nodes.  If it fails (the common case)
 * it falls through to the next choice.
 *
 * Here is the desired flow graph.  Nodes directly below each other imply
 * fallthrough.  Alternatives 1 and 2 have quick checks.  Alternative
 * 3 doesn't have a quick check so we have to call the slow check.
 * Nodes are marked Qn for quick checks and Sn for slow checks.  The entire
 * regexp continuation is generated directly after the Sn node, up to the
 * next JumpOrBacktrack if we decide to reuse some already generated code.  Some
 * nodes expect preload_characters to be preloaded into the current
 * character register.  R nodes do this preloading.  Vertices are marked
 * F for failures and S for success (possible success in the case of quick
 * nodes).  L, V, < and > are used as arrow heads.
 *
 * ----------> R
 *             |
 *             V
 *            Q1 -----> S1
 *             |   S   /
 *            F|      /
 *             |    F/
 *             |    /
 *             |   R
 *             |  /
 *             V L
 *            Q2 -----> S2
 *             |   S   /
 *            F|      /
 *             |    F/
 *             |    /
 *             |   R
 *             |  /
 *             V L
 *            S3
 *             |
 *            F|
 *             |
 *             R
 *             |
 * backtrack   V
 * <----------Q4
 *   \    F    |
 *    \        |S
 *     \   F   V
 *      \-----S4
 *
 * For greedy loops we reverse our expectation and expect to match rather
 * than fail. Therefore we want the loop code to look like this (U is the
 * unwind code that steps back in the greedy loop).  The following alternatives
 * look the same as above.
 *              _____
 *             /     \
 *             V     |
 * ----------> S1    |
 *            /|     |
 *           / |S    |
 *         F/  \_____/
 *         /
 *        |<-----------
 *        |            \
 *        V             \
 *        Q2 ---> S2     \
 *        |  S   /       |
 *       F|     /        |
 *        |   F/         |
 *        |   /          |
 *        |  R           |
 *        | /            |
 *   F    VL             |
 * <------U              |
 * back   |S             |
 *        \______________/
 */

// This class is used when generating the alternatives in a choice node.  It
// records the way the alternative is being code generated.
class irregexp::AlternativeGeneration
{
  public:
    AlternativeGeneration()
      : possible_success(),
        expects_preload(false),
        after(),
        quick_check_details()
    {}

    jit::Label possible_success;
    bool expects_preload;
    jit::Label after;
    QuickCheckDetails quick_check_details;
};

void
ChoiceNode::GenerateGuard(RegExpMacroAssembler* macro_assembler,
                          Guard* guard, Trace* trace)
{
    switch (guard->op()) {
      case Guard::LT:
        MOZ_ASSERT(!trace->mentions_reg(guard->reg()));
        macro_assembler->IfRegisterGE(guard->reg(),
                                      guard->value(),
                                      trace->backtrack());
        break;
      case Guard::GEQ:
        MOZ_ASSERT(!trace->mentions_reg(guard->reg()));
        macro_assembler->IfRegisterLT(guard->reg(),
                                      guard->value(),
                                      trace->backtrack());
        break;
    }
}

int
ChoiceNode::CalculatePreloadCharacters(RegExpCompiler* compiler, int eats_at_least)
{
    int preload_characters = Min(4, eats_at_least);
    if (compiler->macro_assembler()->CanReadUnaligned()) {
        bool ascii = compiler->ascii();
        if (ascii) {
            if (preload_characters > 4)
                preload_characters = 4;
            // We can't preload 3 characters because there is no machine instruction
            // to do that.  We can't just load 4 because we could be reading
            // beyond the end of the string, which could cause a memory fault.
            if (preload_characters == 3)
                preload_characters = 2;
        } else {
            if (preload_characters > 2)
                preload_characters = 2;
        }
    } else {
        if (preload_characters > 1)
            preload_characters = 1;
    }
    return preload_characters;
}

RegExpNode*
TextNode::GetSuccessorOfOmnivorousTextNode(RegExpCompiler* compiler)
{
    if (elements().length() != 1)
        return nullptr;

    TextElement elm = elements()[0];
    if (elm.text_type() != TextElement::CHAR_CLASS)
        return nullptr;

    RegExpCharacterClass* node = elm.char_class();
    CharacterRangeVector& ranges = node->ranges(alloc());

    if (!CharacterRange::IsCanonical(ranges))
        CharacterRange::Canonicalize(ranges);

    if (node->is_negated())
        return ranges.length() == 0 ? on_success() : nullptr;

    if (ranges.length() != 1)
        return nullptr;

    uint32_t max_char = MaximumCharacter(compiler->ascii());
    return ranges[0].IsEverything(max_char) ? on_success() : nullptr;
}

// Finds the fixed match length of a sequence of nodes that goes from
// this alternative and back to this choice node.  If there are variable
// length nodes or other complications in the way then return a sentinel
// value indicating that a greedy loop cannot be constructed.
int
ChoiceNode::GreedyLoopTextLengthForAlternative(GuardedAlternative* alternative)
{
    int length = 0;
    RegExpNode* node = alternative->node();
    // Later we will generate code for all these text nodes using recursion
    // so we have to limit the max number.
    int recursion_depth = 0;
    while (node != this) {
        if (recursion_depth++ > RegExpCompiler::kMaxRecursion) {
            return kNodeIsTooComplexForGreedyLoops;
        }
        int node_length = node->GreedyLoopTextLength();
        if (node_length == kNodeIsTooComplexForGreedyLoops) {
            return kNodeIsTooComplexForGreedyLoops;
        }
        length += node_length;
        SeqRegExpNode* seq_node = static_cast<SeqRegExpNode*>(node);
        node = seq_node->on_success();
    }
    return length;
}

// Creates a list of AlternativeGenerations.  If the list has a reasonable
// size then it is on the stack, otherwise the excess is on the heap.
class AlternativeGenerationList
{
  public:
    AlternativeGenerationList(LifoAlloc* alloc, size_t count)
      : alt_gens_(*alloc)
    {
        alt_gens_.reserve(count);
        for (size_t i = 0; i < count && i < kAFew; i++)
            alt_gens_.append(a_few_alt_gens_ + i);
        for (size_t i = kAFew; i < count; i++) {
            AutoEnterOOMUnsafeRegion oomUnsafe;
            AlternativeGeneration* gen = js_new<AlternativeGeneration>();
            if (!gen)
                oomUnsafe.crash("AlternativeGenerationList js_new");
            alt_gens_.append(gen);
        }
    }

    ~AlternativeGenerationList() {
        for (size_t i = kAFew; i < alt_gens_.length(); i++) {
            js_delete(alt_gens_[i]);
            alt_gens_[i] = nullptr;
        }
    }

    AlternativeGeneration* at(int i) {
        return alt_gens_[i];
    }

  private:
    static const size_t kAFew = 10;
    InfallibleVector<AlternativeGeneration*, 1> alt_gens_;
    AlternativeGeneration a_few_alt_gens_[kAFew];
};

void
ChoiceNode::Emit(RegExpCompiler* compiler, Trace* trace)
{
    RegExpMacroAssembler* macro_assembler = compiler->macro_assembler();
    size_t choice_count = alternatives().length();
#ifdef DEBUG
    for (size_t i = 0; i < choice_count - 1; i++) {
        const GuardedAlternative& alternative = alternatives()[i];
        const GuardVector* guards = alternative.guards();
        if (guards) {
            for (size_t j = 0; j < guards->length(); j++)
                MOZ_ASSERT(!trace->mentions_reg((*guards)[j]->reg()));
        }
    }
#endif

    LimitResult limit_result = LimitVersions(compiler, trace);
    if (limit_result == DONE) return;
    MOZ_ASSERT(limit_result == CONTINUE);

    int new_flush_budget = trace->flush_budget() / choice_count;
    if (trace->flush_budget() == 0 && trace->actions() != nullptr) {
        trace->Flush(compiler, this);
        return;
    }

    RecursionCheck rc(compiler);

    Trace* current_trace = trace;

    int text_length = GreedyLoopTextLengthForAlternative(&alternatives()[0]);
    bool greedy_loop = false;
    jit::Label greedy_loop_label;
    Trace counter_backtrack_trace;
    counter_backtrack_trace.set_backtrack(&greedy_loop_label);
    if (not_at_start()) counter_backtrack_trace.set_at_start(false);

    if (choice_count > 1 && text_length != kNodeIsTooComplexForGreedyLoops) {
        // Here we have special handling for greedy loops containing only text nodes
        // and other simple nodes.  These are handled by pushing the current
        // position on the stack and then incrementing the current position each
        // time around the switch.  On backtrack we decrement the current position
        // and check it against the pushed value.  This avoids pushing backtrack
        // information for each iteration of the loop, which could take up a lot of
        // space.
        greedy_loop = true;
        MOZ_ASSERT(trace->stop_node() == nullptr);
        macro_assembler->PushCurrentPosition();
        current_trace = &counter_backtrack_trace;
        jit::Label greedy_match_failed;
        Trace greedy_match_trace;
        if (not_at_start()) greedy_match_trace.set_at_start(false);
        greedy_match_trace.set_backtrack(&greedy_match_failed);
        jit::Label loop_label;
        macro_assembler->Bind(&loop_label);
        greedy_match_trace.set_stop_node(this);
        greedy_match_trace.set_loop_label(&loop_label);
        alternatives()[0].node()->Emit(compiler, &greedy_match_trace);
        macro_assembler->Bind(&greedy_match_failed);
    }

    jit::Label second_choice;  // For use in greedy matches.
    macro_assembler->Bind(&second_choice);

    size_t first_normal_choice = greedy_loop ? 1 : 0;

    bool not_at_start = current_trace->at_start() == Trace::FALSE_VALUE;
    const int kEatsAtLeastNotYetInitialized = -1;
    int eats_at_least = kEatsAtLeastNotYetInitialized;

    bool skip_was_emitted = false;

    if (!greedy_loop && choice_count == 2) {
        GuardedAlternative alt1 = alternatives()[1];
        if (!alt1.guards() || alt1.guards()->length() == 0) {
            RegExpNode* eats_anything_node = alt1.node();
            if (eats_anything_node->GetSuccessorOfOmnivorousTextNode(compiler) == this) {
                // At this point we know that we are at a non-greedy loop that will eat
                // any character one at a time.  Any non-anchored regexp has such a
                // loop prepended to it in order to find where it starts.  We look for
                // a pattern of the form ...abc... where we can look 6 characters ahead
                // and step forwards 3 if the character is not one of abc.  Abc need
                // not be atoms, they can be any reasonably limited character class or
                // small alternation.
                MOZ_ASSERT(trace->is_trivial());  // This is the case on LoopChoiceNodes.
                BoyerMooreLookahead* lookahead = bm_info(not_at_start);
                if (lookahead == nullptr) {
                    eats_at_least = Min(kMaxLookaheadForBoyerMoore,
                                        EatsAtLeast(kMaxLookaheadForBoyerMoore,
                                                    kRecursionBudget,
                                                    not_at_start));
                    if (eats_at_least >= 1) {
                        BoyerMooreLookahead* bm =
                            alloc()->newInfallible<BoyerMooreLookahead>(alloc(), eats_at_least, compiler);
                        GuardedAlternative alt0 = alternatives()[0];
                        alt0.node()->FillInBMInfo(0, kRecursionBudget, bm, not_at_start);
                        skip_was_emitted = bm->EmitSkipInstructions(macro_assembler);
                    }
                } else {
                    skip_was_emitted = lookahead->EmitSkipInstructions(macro_assembler);
                }
            }
        }
    }

    if (eats_at_least == kEatsAtLeastNotYetInitialized) {
        // Save some time by looking at most one machine word ahead.
        eats_at_least =
            EatsAtLeast(compiler->ascii() ? 4 : 2, kRecursionBudget, not_at_start);
    }
    int preload_characters = CalculatePreloadCharacters(compiler, eats_at_least);

    bool preload_is_current = !skip_was_emitted &&
        (current_trace->characters_preloaded() == preload_characters);
    bool preload_has_checked_bounds = preload_is_current;

    AlternativeGenerationList alt_gens(alloc(), choice_count);

    // For now we just call all choices one after the other.  The idea ultimately
    // is to use the Dispatch table to try only the relevant ones.
    for (size_t i = first_normal_choice; i < choice_count; i++) {
        GuardedAlternative alternative = alternatives()[i];
        AlternativeGeneration* alt_gen = alt_gens.at(i);
        alt_gen->quick_check_details.set_characters(preload_characters);
        const GuardVector* guards = alternative.guards();
        Trace new_trace(*current_trace);
        new_trace.set_characters_preloaded(preload_is_current ?
                                           preload_characters :
                                           0);
        if (preload_has_checked_bounds) {
            new_trace.set_bound_checked_up_to(preload_characters);
        }
        new_trace.quick_check_performed()->Clear();
        if (not_at_start_) new_trace.set_at_start(Trace::FALSE_VALUE);
        alt_gen->expects_preload = preload_is_current;
        bool generate_full_check_inline = false;
        if (try_to_emit_quick_check_for_alternative(i) &&
            alternative.node()->EmitQuickCheck(compiler,
                                               &new_trace,
                                               preload_has_checked_bounds,
                                               &alt_gen->possible_success,
                                               &alt_gen->quick_check_details,
                                               i < choice_count - 1)) {
            // Quick check was generated for this choice.
            preload_is_current = true;
            preload_has_checked_bounds = true;
            // On the last choice in the ChoiceNode we generated the quick
            // check to fall through on possible success.  So now we need to
            // generate the full check inline.
            if (i == choice_count - 1) {
                macro_assembler->Bind(&alt_gen->possible_success);
                new_trace.set_quick_check_performed(&alt_gen->quick_check_details);
                new_trace.set_characters_preloaded(preload_characters);
                new_trace.set_bound_checked_up_to(preload_characters);
                generate_full_check_inline = true;
            }
        } else if (alt_gen->quick_check_details.cannot_match()) {
            if (i == choice_count - 1 && !greedy_loop) {
                macro_assembler->JumpOrBacktrack(trace->backtrack());
            }
            continue;
        } else {
            // No quick check was generated.  Put the full code here.
            // If this is not the first choice then there could be slow checks from
            // previous cases that go here when they fail.  There's no reason to
            // insist that they preload characters since the slow check we are about
            // to generate probably can't use it.
            if (i != first_normal_choice) {
                alt_gen->expects_preload = false;
                new_trace.InvalidateCurrentCharacter();
            }
            if (i < choice_count - 1) {
                new_trace.set_backtrack(&alt_gen->after);
            }
            generate_full_check_inline = true;
        }
        if (generate_full_check_inline) {
            if (new_trace.actions() != nullptr)
                new_trace.set_flush_budget(new_flush_budget);
            if (guards) {
                for (size_t j = 0; j < guards->length(); j++)
                    GenerateGuard(macro_assembler, (*guards)[j], &new_trace);
            }
            alternative.node()->Emit(compiler, &new_trace);
            preload_is_current = false;
        }
        macro_assembler->Bind(&alt_gen->after);
    }
    if (greedy_loop) {
        macro_assembler->Bind(&greedy_loop_label);
        // If we have unwound to the bottom then backtrack.
        macro_assembler->CheckGreedyLoop(trace->backtrack());
        // Otherwise try the second priority at an earlier position.
        macro_assembler->AdvanceCurrentPosition(-text_length);
        macro_assembler->JumpOrBacktrack(&second_choice);
    }

    // At this point we need to generate slow checks for the alternatives where
    // the quick check was inlined.  We can recognize these because the associated
    // label was bound.
    for (size_t i = first_normal_choice; i < choice_count - 1; i++) {
        AlternativeGeneration* alt_gen = alt_gens.at(i);
        Trace new_trace(*current_trace);
        // If there are actions to be flushed we have to limit how many times
        // they are flushed.  Take the budget of the parent trace and distribute
        // it fairly amongst the children.
        if (new_trace.actions() != nullptr) {
            new_trace.set_flush_budget(new_flush_budget);
        }
        EmitOutOfLineContinuation(compiler,
                                  &new_trace,
                                  alternatives()[i],
                                  alt_gen,
                                  preload_characters,
                                  alt_gens.at(i + 1)->expects_preload);
    }
}

void
ChoiceNode::EmitOutOfLineContinuation(RegExpCompiler* compiler,
                                      Trace* trace,
                                      GuardedAlternative alternative,
                                      AlternativeGeneration* alt_gen,
                                      int preload_characters,
                                      bool next_expects_preload)
{
    if (!alt_gen->possible_success.used())
        return;

    RegExpMacroAssembler* macro_assembler = compiler->macro_assembler();
    macro_assembler->Bind(&alt_gen->possible_success);
    Trace out_of_line_trace(*trace);
    out_of_line_trace.set_characters_preloaded(preload_characters);
    out_of_line_trace.set_quick_check_performed(&alt_gen->quick_check_details);
    if (not_at_start_) out_of_line_trace.set_at_start(Trace::FALSE_VALUE);
    const GuardVector* guards = alternative.guards();
    if (next_expects_preload) {
        jit::Label reload_current_char;
        out_of_line_trace.set_backtrack(&reload_current_char);
        if (guards) {
            for (size_t j = 0; j < guards->length(); j++)
                GenerateGuard(macro_assembler, (*guards)[j], &out_of_line_trace);
        }
        alternative.node()->Emit(compiler, &out_of_line_trace);
        macro_assembler->Bind(&reload_current_char);
        // Reload the current character, since the next quick check expects that.
        // We don't need to check bounds here because we only get into this
        // code through a quick check which already did the checked load.
        macro_assembler->LoadCurrentCharacter(trace->cp_offset(),
                                              nullptr,
                                              false,
                                              preload_characters);
        macro_assembler->JumpOrBacktrack(&(alt_gen->after));
    } else {
        out_of_line_trace.set_backtrack(&(alt_gen->after));
        if (guards) {
            for (size_t j = 0; j < guards->length(); j++)
                GenerateGuard(macro_assembler, (*guards)[j], &out_of_line_trace);
        }
        alternative.node()->Emit(compiler, &out_of_line_trace);
    }
}

void
ActionNode::Emit(RegExpCompiler* compiler, Trace* trace)
{
    RegExpMacroAssembler* assembler = compiler->macro_assembler();
    LimitResult limit_result = LimitVersions(compiler, trace);
    if (limit_result == DONE) return;
    MOZ_ASSERT(limit_result == CONTINUE);

    RecursionCheck rc(compiler);

    switch (action_type_) {
      case STORE_POSITION: {
        Trace::DeferredCapture
            new_capture(data_.u_position_register.reg,
                        data_.u_position_register.is_capture,
                        trace);
        Trace new_trace = *trace;
        new_trace.add_action(&new_capture);
        on_success()->Emit(compiler, &new_trace);
        break;
      }
      case INCREMENT_REGISTER: {
        Trace::DeferredIncrementRegister
            new_increment(data_.u_increment_register.reg);
        Trace new_trace = *trace;
        new_trace.add_action(&new_increment);
        on_success()->Emit(compiler, &new_trace);
        break;
      }
      case SET_REGISTER: {
        Trace::DeferredSetRegister
            new_set(data_.u_store_register.reg, data_.u_store_register.value);
        Trace new_trace = *trace;
        new_trace.add_action(&new_set);
        on_success()->Emit(compiler, &new_trace);
        break;
      }
      case CLEAR_CAPTURES: {
        Trace::DeferredClearCaptures
            new_capture(Interval(data_.u_clear_captures.range_from,
                                 data_.u_clear_captures.range_to));
        Trace new_trace = *trace;
        new_trace.add_action(&new_capture);
        on_success()->Emit(compiler, &new_trace);
        break;
      }
      case BEGIN_SUBMATCH:
        if (!trace->is_trivial()) {
            trace->Flush(compiler, this);
        } else {
            assembler->WriteCurrentPositionToRegister(data_.u_submatch.current_position_register, 0);
            assembler->WriteBacktrackStackPointerToRegister(data_.u_submatch.stack_pointer_register);
            on_success()->Emit(compiler, trace);
        }
        break;
      case EMPTY_MATCH_CHECK: {
        int start_pos_reg = data_.u_empty_match_check.start_register;
        int stored_pos = 0;
        int rep_reg = data_.u_empty_match_check.repetition_register;
        bool has_minimum = (rep_reg != RegExpCompiler::kNoRegister);
        bool know_dist = trace->GetStoredPosition(start_pos_reg, &stored_pos);
        if (know_dist && !has_minimum && stored_pos == trace->cp_offset()) {
            // If we know we haven't advanced and there is no minimum we
            // can just backtrack immediately.
            assembler->JumpOrBacktrack(trace->backtrack());
        } else if (know_dist && stored_pos < trace->cp_offset()) {
            // If we know we've advanced we can generate the continuation
            // immediately.
            on_success()->Emit(compiler, trace);
        } else if (!trace->is_trivial()) {
            trace->Flush(compiler, this);
        } else {
            jit::Label skip_empty_check;
            // If we have a minimum number of repetitions we check the current
            // number first and skip the empty check if it's not enough.
            if (has_minimum) {
                int limit = data_.u_empty_match_check.repetition_limit;
                assembler->IfRegisterLT(rep_reg, limit, &skip_empty_check);
            }
            // If the match is empty we bail out, otherwise we fall through
            // to the on-success continuation.
            assembler->IfRegisterEqPos(data_.u_empty_match_check.start_register,
                                       trace->backtrack());
            assembler->Bind(&skip_empty_check);
            on_success()->Emit(compiler, trace);
        }
        break;
      }
      case POSITIVE_SUBMATCH_SUCCESS: {
        if (!trace->is_trivial()) {
            trace->Flush(compiler, this);
            return;
        }
        assembler->ReadCurrentPositionFromRegister(data_.u_submatch.current_position_register);
        assembler->ReadBacktrackStackPointerFromRegister(data_.u_submatch.stack_pointer_register);
        int clear_register_count = data_.u_submatch.clear_register_count;
        if (clear_register_count == 0) {
            on_success()->Emit(compiler, trace);
            return;
        }
        int clear_registers_from = data_.u_submatch.clear_register_from;
        jit::Label clear_registers_backtrack;
        Trace new_trace = *trace;
        new_trace.set_backtrack(&clear_registers_backtrack);
        on_success()->Emit(compiler, &new_trace);

        assembler->Bind(&clear_registers_backtrack);
        int clear_registers_to = clear_registers_from + clear_register_count - 1;
        assembler->ClearRegisters(clear_registers_from, clear_registers_to);

        MOZ_ASSERT(trace->backtrack() == nullptr);
        assembler->Backtrack();
        return;
      }
      default:
        MOZ_CRASH("Bad action");
    }
}

void
BackReferenceNode::Emit(RegExpCompiler* compiler, Trace* trace)
{
    RegExpMacroAssembler* assembler = compiler->macro_assembler();
    if (!trace->is_trivial()) {
        trace->Flush(compiler, this);
        return;
    }

    LimitResult limit_result = LimitVersions(compiler, trace);
    if (limit_result == DONE) return;
    MOZ_ASSERT(limit_result == CONTINUE);

    RecursionCheck rc(compiler);

    MOZ_ASSERT(start_reg_ + 1 == end_reg_);
    if (compiler->ignore_case()) {
        assembler->CheckNotBackReferenceIgnoreCase(start_reg_,
                                                   trace->backtrack(),
                                                   compiler->unicode());
    } else {
        assembler->CheckNotBackReference(start_reg_, trace->backtrack());
    }
    on_success()->Emit(compiler, trace);
}

RegExpNode::LimitResult
RegExpNode::LimitVersions(RegExpCompiler* compiler, Trace* trace)
{
    // If we are generating a greedy loop then don't stop and don't reuse code.
    if (trace->stop_node() != nullptr)
        return CONTINUE;

    RegExpMacroAssembler* macro_assembler = compiler->macro_assembler();
    if (trace->is_trivial()) {
        if (label()->bound()) {
            // We are being asked to generate a generic version, but that's already
            // been done so just go to it.
            macro_assembler->JumpOrBacktrack(label());
            return DONE;
        }
        if (compiler->recursion_depth() >= RegExpCompiler::kMaxRecursion) {
            // To avoid too deep recursion we push the node to the work queue and just
            // generate a goto here.
            compiler->AddWork(this);
            macro_assembler->JumpOrBacktrack(label());
            return DONE;
        }
        // Generate generic version of the node and bind the label for later use.
        macro_assembler->Bind(label());
        return CONTINUE;
    }

    // We are being asked to make a non-generic version.  Keep track of how many
    // non-generic versions we generate so as not to overdo it.
    trace_count_++;
    if (trace_count_ < kMaxCopiesCodeGenerated &&
        compiler->recursion_depth() <= RegExpCompiler::kMaxRecursion) {
        return CONTINUE;
    }

    // If we get here code has been generated for this node too many times or
    // recursion is too deep.  Time to switch to a generic version.  The code for
    // generic versions above can handle deep recursion properly.
    trace->Flush(compiler, this);
    return DONE;
}

bool
RegExpNode::EmitQuickCheck(RegExpCompiler* compiler,
                           Trace* trace,
                           bool preload_has_checked_bounds,
                           jit::Label* on_possible_success,
                           QuickCheckDetails* details,
                           bool fall_through_on_failure)
{
    if (details->characters() == 0) return false;
    GetQuickCheckDetails(
                         details, compiler, 0, trace->at_start() == Trace::FALSE_VALUE);
    if (details->cannot_match()) return false;
    if (!details->Rationalize(compiler->ascii())) return false;
    MOZ_ASSERT(details->characters() == 1 ||
               compiler->macro_assembler()->CanReadUnaligned());
    uint32_t mask = details->mask();
    uint32_t value = details->value();

    RegExpMacroAssembler* assembler = compiler->macro_assembler();

    if (trace->characters_preloaded() != details->characters()) {
        assembler->LoadCurrentCharacter(trace->cp_offset(),
                                        trace->backtrack(),
                                        !preload_has_checked_bounds,
                                        details->characters());
    }

    bool need_mask = true;

    if (details->characters() == 1) {
        // If number of characters preloaded is 1 then we used a byte or 16 bit
        // load so the value is already masked down.
        uint32_t char_mask = MaximumCharacter(compiler->ascii());
        if ((mask & char_mask) == char_mask) need_mask = false;
        mask &= char_mask;
    } else {
        // For 2-character preloads in ASCII mode or 1-character preloads in
        // TWO_BYTE mode we also use a 16 bit load with zero extend.
        if (details->characters() == 2 && compiler->ascii()) {
            if ((mask & 0xffff) == 0xffff) need_mask = false;
        } else if (details->characters() == 1 && !compiler->ascii()) {
            if ((mask & 0xffff) == 0xffff) need_mask = false;
        } else {
            if (mask == 0xffffffff) need_mask = false;
        }
    }

    if (fall_through_on_failure) {
        if (need_mask) {
            assembler->CheckCharacterAfterAnd(value, mask, on_possible_success);
        } else {
            assembler->CheckCharacter(value, on_possible_success);
        }
    } else {
        if (need_mask) {
            assembler->CheckNotCharacterAfterAnd(value, mask, trace->backtrack());
        } else {
            assembler->CheckNotCharacter(value, trace->backtrack());
        }
    }
    return true;
}

bool
TextNode::FillInBMInfo(int initial_offset,
                       int budget,
                       BoyerMooreLookahead* bm,
                       bool not_at_start)
{
    if (!bm->CheckOverRecursed())
        return false;

    if (initial_offset >= bm->length())
        return true;

    int offset = initial_offset;
    int max_char = bm->max_char();
    for (size_t i = 0; i < elements().length(); i++) {
        if (offset >= bm->length()) {
            if (initial_offset == 0)
                set_bm_info(not_at_start, bm);
            return true;
        }
        TextElement text = elements()[i];
        if (text.text_type() == TextElement::ATOM) {
            RegExpAtom* atom = text.atom();
            for (int j = 0; j < atom->length(); j++, offset++) {
                if (offset >= bm->length()) {
                    if (initial_offset == 0)
                        set_bm_info(not_at_start, bm);
                    return true;
                }
                char16_t character = atom->data()[j];
                if (bm->compiler()->ignore_case()) {
                    char16_t chars[kEcma262UnCanonicalizeMaxWidth];
                    int length = GetCaseIndependentLetters(character,
                                                           bm->max_char() == kMaxOneByteCharCode,
                                                           bm->compiler()->unicode(),
                                                           chars);
                    for (int j = 0; j < length; j++)
                        bm->Set(offset, chars[j]);
                } else {
                    if (character <= max_char) bm->Set(offset, character);
                }
            }
        } else {
            MOZ_ASSERT(TextElement::CHAR_CLASS == text.text_type());
            RegExpCharacterClass* char_class = text.char_class();
            const CharacterRangeVector& ranges = char_class->ranges(alloc());
            if (char_class->is_negated()) {
                bm->SetAll(offset);
            } else {
                for (size_t k = 0; k < ranges.length(); k++) {
                    const CharacterRange& range = ranges[k];
                    if (range.from() > max_char)
                        continue;
                    int to = Min(max_char, static_cast<int>(range.to()));
                    bm->SetInterval(offset, Interval(range.from(), to));
                }
            }
            offset++;
        }
    }
    if (offset >= bm->length()) {
        if (initial_offset == 0) set_bm_info(not_at_start, bm);
        return true;
    }
    if (!on_success()->FillInBMInfo(offset,
                                    budget - 1,
                                    bm,
                                    true))   // Not at start after a text node.
        return false;
    if (initial_offset == 0)
        set_bm_info(not_at_start, bm);
    return true;
}

// -------------------------------------------------------------------
// QuickCheckDetails

// Takes the left-most 1-bit and smears it out, setting all bits to its right.
static inline uint32_t
SmearBitsRight(uint32_t v)
{
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v;
}

// Here is the meat of GetQuickCheckDetails (see also the comment on the
// super-class in the .h file).
//
// We iterate along the text object, building up for each character a
// mask and value that can be used to test for a quick failure to match.
// The masks and values for the positions will be combined into a single
// machine word for the current character width in order to be used in
// generating a quick check.
void
TextNode::GetQuickCheckDetails(QuickCheckDetails* details,
                               RegExpCompiler* compiler,
                               int characters_filled_in,
                               bool not_at_start)
{
    MOZ_ASSERT(characters_filled_in < details->characters());
    int characters = details->characters();
    int char_mask = MaximumCharacter(compiler->ascii());

    for (size_t k = 0; k < elements().length(); k++) {
        TextElement elm = elements()[k];
        if (elm.text_type() == TextElement::ATOM) {
            const CharacterVector& quarks = elm.atom()->data();
            for (size_t i = 0; i < (size_t) characters && i < quarks.length(); i++) {
                QuickCheckDetails::Position* pos =
                    details->positions(characters_filled_in);
                char16_t c = quarks[i];
                if (c > char_mask) {
                    // If we expect a non-ASCII character from an ASCII string,
                    // there is no way we can match. Not even case independent
                    // matching can turn an ASCII character into non-ASCII or
                    // vice versa.
                    details->set_cannot_match();
                    pos->determines_perfectly = false;
                    return;
                }
                if (compiler->ignore_case()) {
                    char16_t chars[kEcma262UnCanonicalizeMaxWidth];
                    size_t length = GetCaseIndependentLetters(c, compiler->ascii(),
                                                              compiler->unicode(), chars);
                    MOZ_ASSERT(length != 0);  // Can only happen if c > char_mask (see above).
                    if (length == 1) {
                        // This letter has no case equivalents, so it's nice and simple
                        // and the mask-compare will determine definitely whether we have
                        // a match at this character position.
                        pos->mask = char_mask;
                        pos->value = c;
                        pos->determines_perfectly = true;
                    } else {
                        uint32_t common_bits = char_mask;
                        uint32_t bits = chars[0];
                        for (size_t j = 1; j < length; j++) {
                            uint32_t differing_bits = ((chars[j] & common_bits) ^ bits);
                            common_bits ^= differing_bits;
                            bits &= common_bits;
                        }
                        // If length is 2 and common bits has only one zero in it then
                        // our mask and compare instruction will determine definitely
                        // whether we have a match at this character position.  Otherwise
                        // it can only be an approximate check.
                        uint32_t one_zero = (common_bits | ~char_mask);
                        if (length == 2 && ((~one_zero) & ((~one_zero) - 1)) == 0) {
                            pos->determines_perfectly = true;
                        }
                        pos->mask = common_bits;
                        pos->value = bits;
                    }
                } else {
                    // Don't ignore case.  Nice simple case where the mask-compare will
                    // determine definitely whether we have a match at this character
                    // position.
                    pos->mask = char_mask;
                    pos->value = c;
                    pos->determines_perfectly = true;
                }
                characters_filled_in++;
                MOZ_ASSERT(characters_filled_in <= details->characters());
                if (characters_filled_in == details->characters()) {
                    return;
                }
            }
        } else {
            QuickCheckDetails::Position* pos =
                details->positions(characters_filled_in);
            RegExpCharacterClass* tree = elm.char_class();
            const CharacterRangeVector& ranges = tree->ranges(alloc());
            if (tree->is_negated()) {
                // A quick check uses multi-character mask and compare.  There is no
                // useful way to incorporate a negative char class into this scheme
                // so we just conservatively create a mask and value that will always
                // succeed.
                pos->mask = 0;
                pos->value = 0;
            } else {
                size_t first_range = 0;
                while (ranges[first_range].from() > char_mask) {
                    first_range++;
                    if (first_range == ranges.length()) {
                        details->set_cannot_match();
                        pos->determines_perfectly = false;
                        return;
                    }
                }
                CharacterRange range = ranges[first_range];
                char16_t from = range.from();
                char16_t to = range.to();
                if (to > char_mask) {
                    to = char_mask;
                }
                uint32_t differing_bits = (from ^ to);
                // A mask and compare is only perfect if the differing bits form a
                // number like 00011111 with one single block of trailing 1s.
                if ((differing_bits & (differing_bits + 1)) == 0 &&
                    from + differing_bits == to) {
                    pos->determines_perfectly = true;
                }
                uint32_t common_bits = ~SmearBitsRight(differing_bits);
                uint32_t bits = (from & common_bits);
                for (size_t i = first_range + 1; i < ranges.length(); i++) {
                    CharacterRange range = ranges[i];
                    char16_t from = range.from();
                    char16_t to = range.to();
                    if (from > char_mask) continue;
                    if (to > char_mask) to = char_mask;
                    // Here we are combining more ranges into the mask and compare
                    // value.  With each new range the mask becomes more sparse and
                    // so the chances of a false positive rise.  A character class
                    // with multiple ranges is assumed never to be equivalent to a
                    // mask and compare operation.
                    pos->determines_perfectly = false;
                    uint32_t new_common_bits = (from ^ to);
                    new_common_bits = ~SmearBitsRight(new_common_bits);
                    common_bits &= new_common_bits;
                    bits &= new_common_bits;
                    uint32_t differing_bits = (from & common_bits) ^ bits;
                    common_bits ^= differing_bits;
                    bits &= common_bits;
                }
                pos->mask = common_bits;
                pos->value = bits;
            }
            characters_filled_in++;
            MOZ_ASSERT(characters_filled_in <= details->characters());
            if (characters_filled_in == details->characters()) {
                return;
            }
        }
    }
    MOZ_ASSERT(characters_filled_in != details->characters());
    if (!details->cannot_match()) {
        on_success()-> GetQuickCheckDetails(details,
                                            compiler,
                                            characters_filled_in,
                                            true);
    }
}

void
QuickCheckDetails::Clear()
{
    for (int i = 0; i < characters_; i++) {
        positions_[i].mask = 0;
        positions_[i].value = 0;
        positions_[i].determines_perfectly = false;
    }
    characters_ = 0;
}

void
QuickCheckDetails::Advance(int by, bool ascii)
{
    MOZ_ASSERT(by >= 0);
    if (by >= characters_) {
        Clear();
        return;
    }
    for (int i = 0; i < characters_ - by; i++) {
        positions_[i] = positions_[by + i];
    }
    for (int i = characters_ - by; i < characters_; i++) {
        positions_[i].mask = 0;
        positions_[i].value = 0;
        positions_[i].determines_perfectly = false;
    }
    characters_ -= by;
    // We could change mask_ and value_ here but we would never advance unless
    // they had already been used in a check and they won't be used again because
    // it would gain us nothing.  So there's no point.
}

bool
QuickCheckDetails::Rationalize(bool is_ascii)
{
    bool found_useful_op = false;
    uint32_t char_mask = MaximumCharacter(is_ascii);

    mask_ = 0;
    value_ = 0;
    int char_shift = 0;
    for (int i = 0; i < characters_; i++) {
        Position* pos = &positions_[i];
        if ((pos->mask & kMaxOneByteCharCode) != 0)
            found_useful_op = true;
        mask_ |= (pos->mask & char_mask) << char_shift;
        value_ |= (pos->value & char_mask) << char_shift;
        char_shift += is_ascii ? 8 : 16;
    }
    return found_useful_op;
}

void QuickCheckDetails::Merge(QuickCheckDetails* other, int from_index)
{
    MOZ_ASSERT(characters_ == other->characters_);
    if (other->cannot_match_)
        return;
    if (cannot_match_) {
        *this = *other;
        return;
    }
    for (int i = from_index; i < characters_; i++) {
        QuickCheckDetails::Position* pos = positions(i);
        QuickCheckDetails::Position* other_pos = other->positions(i);
        if (pos->mask != other_pos->mask ||
            pos->value != other_pos->value ||
            !other_pos->determines_perfectly) {
            // Our mask-compare operation will be approximate unless we have the
            // exact same operation on both sides of the alternation.
            pos->determines_perfectly = false;
        }
        pos->mask &= other_pos->mask;
        pos->value &= pos->mask;
        other_pos->value &= pos->mask;
        char16_t differing_bits = (pos->value ^ other_pos->value);
        pos->mask &= ~differing_bits;
        pos->value &= pos->mask;
    }
}
