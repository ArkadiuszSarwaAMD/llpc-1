﻿/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2020 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  vkgcElfReader.cpp
 * @brief VKGC source file: contains implementation of VKGC ELF reading utilities.
 ***********************************************************************************************************************
 */
#include <algorithm>
#include "vkgcElfReader.h"
#include <string.h>

#define DEBUG_TYPE "vkgc-elf-reader"

using namespace llvm;

namespace Vkgc {

// =====================================================================================================================
//
// @param gfxIp : Graphics IP version info
template <class Elf>
ElfReader<Elf>::ElfReader(GfxIpVersion gfxIp)
    : m_gfxIp(gfxIp), m_header(), m_symSecIdx(InvalidValue), m_relocSecIdx(InvalidValue), m_strtabSecIdx(InvalidValue),
      m_textSecIdx(InvalidValue) {
}

// =====================================================================================================================
template <class Elf> ElfReader<Elf>::~ElfReader() {
  for (auto section : m_sections)
    delete section;
  m_sections.clear();
}

// =====================================================================================================================
// Reads ELF data in from the given buffer into the context.
//
// ELF data is stored in the buffer like so:
//
// + ELF header
// + Section Header String Table
//
// + Section Buffer (b0) [NULL]
// + Section Buffer (b1) [.shstrtab]
// + ...            (b#) [...]
//
// + Section Header (h0) [NULL]
// + Section Header (h1) [.shstrtab]
// + ...            (h#) [...]
//
// @param pBuffer : Input ELF data buffer
// @param [out] pBufSize : Size of the given read buffer (determined from the ELF header)
template <class Elf> Result ElfReader<Elf>::ReadFromBuffer(const void *pBuffer, size_t *pBufSize) {
  assert(pBuffer);

  Result result = Result::Success;

  const uint8_t *data = static_cast<const uint8_t *>(pBuffer);

  // ELF header is always located at the beginning of the file
  auto header = static_cast<const typename Elf::FormatHeader *>(pBuffer);

  // If the identification info isn't the magic number, this isn't a valid file.
  result = header->eIdent32[EI_MAG0] == ElfMagic ? Result::Success : Result::ErrorInvalidValue;

  if (result == Result::Success)
    result = header->eMachine == EM_AMDGPU ? Result::Success : Result::ErrorInvalidValue;

  if (result == Result::Success) {
    m_header = *header;
    size_t readSize = sizeof(typename Elf::FormatHeader);

    // Section header location information.
    const unsigned sectionHeaderOffset = static_cast<unsigned>(header->eShoff);
    const unsigned sectionHeaderNum = header->eShnum;
    const unsigned sectionHeaderSize = header->eShentsize;

    const unsigned sectionStrTableHeaderOffset = sectionHeaderOffset + (header->eShstrndx * sectionHeaderSize);
    auto sectionStrTableHeader =
        reinterpret_cast<const typename Elf::SectionHeader *>(data + sectionStrTableHeaderOffset);
    const unsigned sectionStrTableOffset = static_cast<unsigned>(sectionStrTableHeader->shOffset);

    for (unsigned section = 0; section < sectionHeaderNum; section++) {
      // Where the header is located for this section
      const unsigned sectionOffset = sectionHeaderOffset + (section * sectionHeaderSize);
      auto sectionHeader = reinterpret_cast<const typename Elf::SectionHeader *>(data + sectionOffset);
      readSize += sizeof(typename Elf::SectionHeader);

      // Where the name is located for this section
      const unsigned sectionNameOffset = sectionStrTableOffset + sectionHeader->shName;
      const char *sectionName = reinterpret_cast<const char *>(data + sectionNameOffset);

      // Where the data is located for this section
      const unsigned sectionDataOffset = static_cast<unsigned>(sectionHeader->shOffset);
      auto buf = new SectionBuffer;

      result = buf ? Result::Success : Result::ErrorOutOfMemory;

      if (result == Result::Success) {
        buf->secHead = *sectionHeader;
        buf->name = sectionName;
        buf->data = (data + sectionDataOffset);

        readSize += static_cast<size_t>(sectionHeader->shSize);

        m_sections.push_back(buf);
        m_map[sectionName] = section;
      }
    }

    *pBufSize = readSize;
  }

  // Get section index
  m_symSecIdx = GetSectionIndex(SymTabName);
  m_relocSecIdx = GetSectionIndex(RelocName);
  m_strtabSecIdx = GetSectionIndex(StrTabName);
  m_textSecIdx = GetSectionIndex(TextName);

  return result;
}

// =====================================================================================================================
// Retrieves the section data for the specified section name, if it exists.
//
// @param pName : Name of the section to look for
// @param [out] sectData : Pointer to section data
// @param [out] pDataLength : Size of the section data
template <class Elf>
Result ElfReader<Elf>::GetSectionData(const char *pName, const void **sectData, size_t *pDataLength) const {
  Result result = Result::ErrorInvalidValue;

  auto entry = m_map.find(pName);

  if (entry != m_map.end()) {
    *sectData = m_sections[entry->second]->data;
    *pDataLength = static_cast<size_t>(m_sections[entry->second]->secHead.shSize);
    result = Result::Success;
  }

  return result;
}

// =====================================================================================================================
// Gets the count of symbols in the symbol table section.
template <class Elf> unsigned ElfReader<Elf>::getSymbolCount() const {
  unsigned symCount = 0;
  if (m_symSecIdx >= 0) {
    auto &section = m_sections[m_symSecIdx];
    symCount = static_cast<unsigned>(section->secHead.shSize / section->secHead.shEntsize);
  }
  return symCount;
}

// =====================================================================================================================
// Gets info of the symbol in the symbol table section according to the specified index.
//
// @param idx : Symbol index
// @param [out] pSymbol : Info of the symbol
template <class Elf> void ElfReader<Elf>::getSymbol(unsigned idx, ElfSymbol *pSymbol) {
  auto &section = m_sections[m_symSecIdx];
  const char *strTab = reinterpret_cast<const char *>(m_sections[m_strtabSecIdx]->data);

  auto symbols = reinterpret_cast<const typename Elf::Symbol *>(section->data);
  pSymbol->secIdx = symbols[idx].stShndx;
  pSymbol->secName = m_sections[pSymbol->secIdx]->name;
  pSymbol->pSymName = strTab + symbols[idx].stName;
  pSymbol->size = symbols[idx].stSize;
  pSymbol->value = symbols[idx].stValue;
  pSymbol->info.all = symbols[idx].stInfo.all;
}

// =====================================================================================================================
// Gets the count of relocations in the relocation section.
template <class Elf> unsigned ElfReader<Elf>::getRelocationCount() {
  unsigned relocCount = 0;
  if (m_relocSecIdx >= 0) {
    auto &section = m_sections[m_relocSecIdx];
    relocCount = static_cast<unsigned>(section->secHead.shSize / section->secHead.shEntsize);
  }
  return relocCount;
}

// =====================================================================================================================
// Gets info of the relocation in the relocation section according to the specified index.
//
// @param idx : Relocation index
// @param [out] pReloc : Info of the relocation
template <class Elf> void ElfReader<Elf>::getRelocation(unsigned idx, ElfReloc *pReloc) {
  auto &section = m_sections[m_relocSecIdx];

  auto relocs = reinterpret_cast<const typename Elf::Reloc *>(section->data);
  pReloc->offset = relocs[idx].rOffset;
  pReloc->symIdx = relocs[idx].rSymbol;
}

// =====================================================================================================================
// Gets the count of Elf section.
template <class Elf> unsigned ElfReader<Elf>::getSectionCount() {
  return static_cast<unsigned>(m_sections.size());
}

// =====================================================================================================================
// Gets section data by section index.
//
// @param secIdx : Section index
// @param [out] ppSectionData : Section data
template <class Elf>
Result ElfReader<Elf>::getSectionDataBySectionIndex(unsigned secIdx, SectionBuffer **ppSectionData) const {
  Result result = Result::ErrorInvalidValue;
  if (secIdx < m_sections.size()) {
    *ppSectionData = m_sections[secIdx];
    result = Result::Success;
  }
  return result;
}

// =====================================================================================================================
// Gets section data by sorting index (ordered map).
//
// @param sortIdx : Sorting index
// @param [out] pSecIdx : Section index
// @param [out] ppSectionData : Section data
template <class Elf>
Result ElfReader<Elf>::getSectionDataBySortingIndex(unsigned sortIdx, unsigned *pSecIdx,
                                                    SectionBuffer **ppSectionData) const {
  Result result = Result::ErrorInvalidValue;
  if (sortIdx < m_sections.size()) {
    auto it = m_map.begin();
    for (unsigned i = 0; i < sortIdx; ++i)
      ++it;
    *pSecIdx = it->second;
    *ppSectionData = m_sections[it->second];
    result = Result::Success;
  }
  return result;
}

// =====================================================================================================================
// Gets all associated symbols by section index.
//
// @param secIdx : Section index
// @param [out] secSymbols : ELF symbols
template <class Elf>
void ElfReader<Elf>::GetSymbolsBySectionIndex(unsigned secIdx, std::vector<ElfSymbol> &secSymbols) const {
  if (secIdx < m_sections.size() && m_symSecIdx >= 0) {
    auto &section = m_sections[m_symSecIdx];
    const char *strTab = reinterpret_cast<const char *>(m_sections[m_strtabSecIdx]->data);

    auto symbols = reinterpret_cast<const typename Elf::Symbol *>(section->data);
    unsigned symCount = getSymbolCount();
    ElfSymbol symbol = {};

    for (unsigned idx = 0; idx < symCount; ++idx) {
      if (symbols[idx].stShndx == secIdx) {
        symbol.secIdx = symbols[idx].stShndx;
        symbol.secName = m_sections[symbol.secIdx]->name;
        symbol.pSymName = strTab + symbols[idx].stName;
        symbol.size = symbols[idx].stSize;
        symbol.value = symbols[idx].stValue;
        symbol.info.all = symbols[idx].stInfo.all;

        secSymbols.push_back(symbol);
      }
    }

    std::sort(secSymbols.begin(), secSymbols.end(),
              [](const ElfSymbol &a, const ElfSymbol &b) { return a.value < b.value; });
  }
}

// =====================================================================================================================
// Checks whether the input name is a valid symbol.
//
// @param pSymbolName : Symbol name
template <class Elf> bool ElfReader<Elf>::isValidSymbol(const char *pSymbolName) {
  auto &section = m_sections[m_symSecIdx];
  const char *strTab = reinterpret_cast<const char *>(m_sections[m_strtabSecIdx]->data);

  auto symbols = reinterpret_cast<const typename Elf::Symbol *>(section->data);
  unsigned symCount = getSymbolCount();
  bool findSymbol = false;
  for (unsigned idx = 0; idx < symCount; ++idx) {
    auto name = strTab + symbols[idx].stName;
    if (strcmp(name, pSymbolName) == 0) {
      findSymbol = true;
      break;
    }
  }
  return findSymbol;
}

// =====================================================================================================================
// Gets note according to note type
//
// @param noteType : Note type
template <class Elf> ElfNote ElfReader<Elf>::getNote(Util::Abi::PipelineAbiNoteType noteType) const {
  unsigned noteSecIdx = m_map.at(NoteName);
  assert(noteSecIdx > 0);

  auto noteSection = m_sections[noteSecIdx];
  ElfNote noteNode = {};
  const unsigned noteHeaderSize = sizeof(NoteHeader) - 8;

  size_t offset = 0;
  while (offset < noteSection->secHead.shSize) {
    const NoteHeader *note = reinterpret_cast<const NoteHeader *>(noteSection->data + offset);
    const unsigned noteNameSize = alignTo(note->nameSize, 4);
    if (note->type == noteType) {
      memcpy(&noteNode.hdr, note, sizeof(NoteHeader));
      noteNode.data = noteSection->data + offset + noteHeaderSize + noteNameSize;
      break;
    }
    offset += noteHeaderSize + noteNameSize + alignTo(note->descSize, 4);
  }

  return noteNode;
}

// =====================================================================================================================
// Initialize MsgPack document and related visitor iterators
//
// @param pBuffer : Message buffer
// @param sizeInBytes : Buffer size in bytes
template <class Elf> void ElfReader<Elf>::initMsgPackDocument(const void *pBuffer, unsigned sizeInBytes) {
  m_document.readFromBlob(StringRef(reinterpret_cast<const char *>(pBuffer), sizeInBytes), false);

  auto root = &m_document.getRoot();
  assert(root->isMap());

  m_iteratorStack.clear();
  MsgPackIterator iter = {};
  iter.node = &(root->getMap(true));
  iter.status = MsgPackIteratorMapBegin;
  m_iteratorStack.push_back(iter);

  m_msgPackMapLevel = 0;
}

// =====================================================================================================================
// Advances the MsgPack context to the next item token and return true if success.
template <class Elf> bool ElfReader<Elf>::getNextMsgNode() {
  if (m_iteratorStack.empty())
    return false;

  MsgPackIterator curIter = m_iteratorStack.back();
  bool skipPostCheck = false;
  if (curIter.status == MsgPackIteratorMapBegin) {
    auto map = &(curIter.node->getMap(true));
    curIter.mapIt = map->begin();
    curIter.mapEnd = map->end();
    m_msgPackMapLevel++;
    curIter.status = MsgPackIteratorMapPair;
    m_iteratorStack.push_back(curIter);
    skipPostCheck = true;
  } else if (curIter.status == MsgPackIteratorMapPair) {
    assert(curIter.mapIt->first.isMap() == false && curIter.mapIt->first.isArray() == false);
    curIter.status = MsgPackIteratorMapKey;
    m_iteratorStack.push_back(curIter);
  } else if (curIter.status == MsgPackIteratorMapKey) {
    if (curIter.mapIt->second.isMap()) {
      curIter.status = MsgPackIteratorMapBegin;
      curIter.node = &(curIter.mapIt->second.getMap(true));
    } else if (curIter.mapIt->second.isArray()) {
      curIter.status = MsgPackIteratorArray;
      auto array = &(curIter.mapIt->second.getArray(true));
      curIter.arrayIt = array->begin();
      curIter.arrayEnd = array->end();
      curIter.node = array;
    } else
      curIter.status = MsgPackIteratorMapValue;
    m_iteratorStack.back() = curIter;
    skipPostCheck = true;
  } else if (curIter.status == MsgPackIteratorArray) {
    curIter.arrayIt = curIter.node->getArray(true).begin();
    curIter.arrayEnd = curIter.node->getArray(true).end();
    if (curIter.arrayIt->isMap()) {
      curIter.status = MsgPackIteratorMapBegin;
      curIter.node = &(curIter.arrayIt->getMap(true));
    } else if (curIter.arrayIt->isArray()) {
      curIter.status = MsgPackIteratorArray;
      auto array = &(curIter.arrayIt->getArray(true));
      curIter.arrayIt = array->begin();
      curIter.arrayEnd = array->end();
      curIter.node = array;
    } else
      curIter.status = MsgPackIteratorArrayValue;
    m_iteratorStack.push_back(curIter);
    skipPostCheck = true;
  } else if (curIter.status == MsgPackIteratorMapValue)
    m_iteratorStack.pop_back();
  else if (curIter.status == MsgPackIteratorArrayValue)
    m_iteratorStack.pop_back();
  else if (curIter.status == MsgPackIteratorMapEnd) {
    m_iteratorStack.pop_back();
    m_iteratorStack.pop_back();
    m_msgPackMapLevel--;
  } else if (curIter.status == MsgPackIteratorArrayEnd) {
    m_iteratorStack.pop_back();
    m_iteratorStack.pop_back();
  }

  // Post check for end visit map or array element
  if (m_iteratorStack.size() > 0 && !skipPostCheck) {
    auto nextIter = &m_iteratorStack.back();
    if (nextIter->status == MsgPackIteratorMapPair) {
      nextIter->mapIt++;
      if (nextIter->mapIt == nextIter->mapEnd)
        nextIter->status = MsgPackIteratorMapEnd;
    } else if (nextIter->status == MsgPackIteratorArray) {
      nextIter->arrayIt++;
      curIter = *nextIter;
      if (curIter.arrayIt == curIter.arrayEnd)
        curIter.status = MsgPackIteratorArrayEnd;
      else if (curIter.arrayIt->isMap()) {
        curIter.status = MsgPackIteratorMapBegin;
        curIter.node = &(curIter.arrayIt->getMap(true));
      } else if (curIter.arrayIt->isArray()) {
        curIter.status = MsgPackIteratorArray;
        auto array = &(curIter.arrayIt->getArray(true));
        curIter.arrayIt = array->begin();
        curIter.arrayEnd = array->end();
        curIter.node = array;
      } else
        curIter.status = MsgPackIteratorArrayValue;
      m_iteratorStack.push_back(curIter);
    }
  }

  return m_iteratorStack.size() > 0;
}

// =====================================================================================================================
// Gets MsgPack node.
template <class Elf> const llvm::msgpack::DocNode *ElfReader<Elf>::getMsgNode() const {
  assert(m_iteratorStack.size() > 0);
  auto curIter = &(m_iteratorStack.back());
  if (curIter->status == MsgPackIteratorArrayValue)
    return &(*curIter->arrayIt);
  else if (curIter->status == MsgPackIteratorMapValue)
    return &(curIter->mapIt->second);
  else if (curIter->status == MsgPackIteratorMapKey)
    return &(curIter->mapIt->first);
  else
    return curIter->node;
}

// =====================================================================================================================
// Gets the map level of current message item.
template <class Elf> unsigned ElfReader<Elf>::getMsgMapLevel() const {
  return m_msgPackMapLevel;
}

// =====================================================================================================================
// Gets the status of message packer iterator.
template <class Elf> MsgPackIteratorStatus ElfReader<Elf>::getMsgIteratorStatus() const {
  return m_iteratorStack.back().status;
}

// Explicit instantiations for ELF utilities
template class ElfReader<Elf64>;

} // namespace Vkgc
