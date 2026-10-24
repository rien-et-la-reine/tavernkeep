// SPDX-FileCopyrightText: 2026 Harsh Patel
// SPDX-License-Identifier: MIT

#ifndef BARTAP_XML_H
#define BARTAP_XML_H

/* DRAFT INTERFACE — not final. Open questions are numbered in
 * DESIGN-QUESTIONS.md (Q1, Q2, ...) and referred to below.
 *
 * XML: a streaming, non-validating XML tokenizer. Internal to Bartap (D9 in
 * DECISIONS.md), used for META-INF/container.xml, the OPF package and the
 * navigation document or NCX, and for content documents too if Q17 decides
 * Bartap reads them.
 *
 * Intended properties:
 * - Streaming: input arrives in pieces through the OCF layer's bounded reads,
 *   and the tokenizer resumes where it stopped, so no document has to be held
 *   whole in memory.
 * - Bounded work per call (D5).
 * - Input is untrusted (D6), so it enforces limits on nesting depth, name
 *   length and token length, and refuses a document that exceeds them.
 * - UTF-8 input. How UTF-16 documents, which EPUB allows for content
 *   documents, are handled is open.
 *
 * TODO: the interface itself is undesigned, so this header declares nothing
 * yet. Its shape is Q20. */

#endif
