/*
 * Copyright 2011 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Author: morlovich@google.com (Maksim Orlovich)
//
// Implementation of JsCombineFilter class which combines multiple external JS
// scripts into a single one. JsCombineFilter contains logic to decide when to
// combine based on the HTML event stream, while the actual combining and
// content-based vetoing is delegated to the JsCombineFilter::JsCombiner helper.
// That in turn largely relies on the common logic in its parent classes to
// deal with resource management.

#include "net/instaweb/rewriter/public/js_combine_filter.h"

#include <vector>

#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "net/instaweb/htmlparse/public/html_element.h"
#include "net/instaweb/htmlparse/public/html_name.h"
#include "net/instaweb/htmlparse/public/html_node.h"
#include "net/instaweb/http/public/content_type.h"
#include "net/instaweb/rewriter/cached_result.pb.h"
#include "net/instaweb/rewriter/public/javascript_code_block.h"
#include "net/instaweb/rewriter/public/output_resource.h"
#include "net/instaweb/rewriter/public/output_resource_kind.h"
#include "net/instaweb/rewriter/public/resource.h"
#include "net/instaweb/rewriter/public/resource_combiner.h"
#include "net/instaweb/rewriter/public/resource_manager.h"
#include "net/instaweb/rewriter/public/resource_slot.h"
#include "net/instaweb/rewriter/public/rewrite_context.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_result.h"
#include "net/instaweb/rewriter/public/script_tag_scanner.h"
#include "net/instaweb/util/public/basictypes.h"
#include "net/instaweb/util/public/ref_counted_ptr.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/string.h"
#include "net/instaweb/util/public/string_util.h"
#include "net/instaweb/util/public/url_multipart_encoder.h"
#include "net/instaweb/util/public/writer.h"

namespace net_instaweb {

class MessageHandler;
class UrlSegmentEncoder;

const char JsCombineFilter::kJsFileCountReduction[] = "js_file_count_reduction";

// See file comment and ResourceCombiner docs for this class's role.
class JsCombineFilter::JsCombiner : public ResourceCombiner {
 public:
  JsCombiner(JsCombineFilter* filter, RewriteDriver* driver)
      : ResourceCombiner(driver, kContentTypeJavascript.file_extension() + 1,
                         filter),
        filter_(filter) {
    Statistics* stats = resource_manager_->statistics();
    js_file_count_reduction_ = stats->GetVariable(kJsFileCountReduction);
  }

  virtual ~JsCombiner() {
  }

  virtual bool ResourceCombinable(Resource* resource, MessageHandler* handler) {
    // In strict mode of ES262-5 eval runs in a private variable scope,
    // (see 10.4.2 step 3 and 10.4.2.1), so our transformation is not safe.
    // Strict mode is identified by 'use strict' or "use strict" string literals
    // (escape-free) in some contexts. As a conservative approximation, we just
    // look for the text
    if (resource->contents().find("use strict") != StringPiece::npos) {
      return false;
    }
    const RewriteOptions* options = rewrite_driver_->options();
    if (options->avoid_renaming_introspective_javascript() &&
        JavascriptCodeBlock::UnsafeToRename(resource->contents())) {
      return false;
    }

    // TODO(morlovich): define a pragma that javascript authors can
    // include in their source to prevent inclusion in a js combination
    return true;
  }

  // This eventually calls WritePiece().
  bool Write(const ResourceVector& in, const OutputResourcePtr& out) {
    return WriteCombination(in, out, rewrite_driver_->message_handler());
  }

  // Create the output resource for this combination.
  OutputResourcePtr MakeOutput() {
    return Combine(rewrite_driver_->message_handler());
  }

  // Stats.
  void AddFileCountReduction(int files) {
    js_file_count_reduction_->Add(files);
  }

 private:
  virtual const ContentType* CombinationContentType() {
    return &kContentTypeJavascript;
  }

  virtual bool WritePiece(int index, const Resource* input,
                          OutputResource* combination, Writer* writer,
                          MessageHandler* handler);

  JsCombineFilter* filter_;
  Variable* js_file_count_reduction_;

  DISALLOW_COPY_AND_ASSIGN(JsCombiner);
};

class JsCombineFilter::Context : public RewriteContext {
 public:
  Context(RewriteDriver* driver, JsCombineFilter* filter)
      : RewriteContext(driver, NULL, NULL),
        combiner_(filter, driver),
        filter_(filter),
        fresh_combination_(true) {
  }

  // Create and add the slot that corresponds to this element.
  bool AddElement(HtmlElement* element, HtmlElement::Attribute* href) {
    bool ret = true;
    ResourcePtr resource(filter_->CreateInputResource(
        href->DecodedValueOrNull()));
    if (resource.get() != NULL) {
      ResourceSlotPtr slot(Driver()->GetSlot(resource, element, href));
      AddSlot(slot);
      elements_.push_back(element);
      fresh_combination_ = false;
    } else {
      ret = false;
    }
    return ret;
  }

  // If we get a flush in the middle of things, we may have put a
  // script tag on that now can't be re-written and should be removed
  // from the combination.  Remove the corresponding slot as well,
  // because we are no longer handling the resource associated with it.
  void RemoveLastElement() {
    RemoveLastSlot();
    elements_.resize(elements_.size() - 1);
  }

  bool HasElementLast(HtmlElement* element) {
    return !empty() && elements_[elements_.size() - 1] == element;
  }

  JsCombiner* combiner() { return &combiner_; }
  bool empty() const { return elements_.empty(); }
  bool fresh_combination() { return fresh_combination_; }

  void Reset() {
    fresh_combination_ = true;
    combiner_.Reset();
  }

 protected:
  // Divide the slots into partitions according to which js files can
  // be combined together.
  virtual bool Partition(OutputPartitions* partitions,
                         OutputResourceVector* outputs) {
    MessageHandler* handler = Driver()->message_handler();
    CachedResult* partition = NULL;
    CHECK_EQ(static_cast<int>(elements_.size()), num_slots());

    // For each slot, try to add its resource to the current partition.
    // If we can't, then finalize the last combination, and then
    // move on to the next slot.
    for (int i = 0, n = num_slots(); i < n; ++i) {
      bool add_input = false;
      ResourcePtr resource(slot(i)->resource());
      if (resource->IsValidAndCacheable()) {
        if (combiner_.AddResourceNoFetch(resource, handler).value) {
          add_input = true;
        } else if (partition != NULL) {
          FinalizePartition(partitions, partition, outputs);
          partition = NULL;
          if (combiner_.AddResourceNoFetch(resource, handler).value) {
            add_input = true;
          }
        }
      } else {
        FinalizePartition(partitions, partition, outputs);
        partition = NULL;
      }
      if (add_input) {
        if (partition == NULL) {
          partition = partitions->add_partition();
        }
        resource->AddInputInfoToPartition(
            Resource::kIncludeInputHash, i, partition);
      }
    }
    FinalizePartition(partitions, partition, outputs);
    return (partitions->partition_size() != 0);
  }

  // Actually write the new resource.
  virtual void Rewrite(int partition_index,
                       CachedResult* partition,
                       const OutputResourcePtr& output) {
    RewriteResult result = kRewriteOk;
    if (!output->IsWritten()) {
      ResourceVector resources;
      for (int i = 0, n = num_slots(); i < n; ++i) {
        ResourcePtr resource(slot(i)->resource());
        resources.push_back(resource);
      }
      if (!combiner_.Write(resources, output)) {
        result = kRewriteFailed;
      }
    }
    RewriteDone(result, partition_index);
  }

  // For every partition, write a new script tag that points to the
  // combined resource.  Then create new script tags for each slot
  // in the partition that evaluate the variable that refers to the
  // original script for that tag.
  virtual void Render() {
    for (int p = 0, np = num_output_partitions(); p < np; ++p) {
      CachedResult* partition = output_partition(p);
      int partition_size = partition->input_size();
      if (partition_size > 1) {
        // Make sure we can edit every element here.
        bool can_rewrite = true;
        for (int i = 0; i < partition_size; ++i) {
          int slot_index = partition->input(i).index();
          HtmlResourceSlot* html_slot =
              static_cast<HtmlResourceSlot*>(slot(slot_index).get());
          if (!Driver()->IsRewritable(html_slot->element())) {
            can_rewrite = false;
          }
        }

        if (can_rewrite) {
          MakeCombinedElement(partition);
          // we still need to add eval() in place of the
          // other slots.
          for (int i = 0; i < partition_size; ++i) {
            int slot_index = partition->input(i).index();
            MakeScriptElement(slot_index);
          }
          combiner_.AddFileCountReduction(partition_size - 1);
        } else {
          // Make sure we don't change any of the URLs.
          for (int i = 0; i < partition_size; ++i) {
            slot(partition->input(i).index())->set_disable_rendering(true);
          }
        }  // if (can_rewrite)
      }  // if (partition_size > 1)
    }
  }

  virtual const UrlSegmentEncoder* encoder() const {
    return filter_->encoder();
  }
  virtual const char* id() const { return filter_->id(); }
  virtual OutputResourceKind kind() const { return kRewrittenResource; }

 private:
  // If we can combine, put the result into outputs and then reset
  // the context (and the combiner) so we start with a fresh slate
  // for any new slots.
  void FinalizePartition(OutputPartitions* partitions,
                         CachedResult* partition,
                         OutputResourceVector* outputs) {
    if (partition != NULL) {
      OutputResourcePtr combination_output(combiner_.MakeOutput());
      if (combination_output.get() == NULL) {
        partitions->mutable_partition()->RemoveLast();
      } else {
        combination_output->UpdateCachedResultPreservingInputInfo(partition);
        outputs->push_back(combination_output);
      }
      Reset();
    }
  }

  // Create an element for the combination of all the elements in the
  // partition. Insert it before first one.
  void MakeCombinedElement(CachedResult* partition) {
    int first_index = partition->input(0).index();
    HtmlResourceSlot* first_slot =
        static_cast<HtmlResourceSlot*>(slot(first_index).get());
    HtmlElement* combine_element =
        Driver()->NewElement(NULL,  // no parent yet.
                             HtmlName::kScript);
    Driver()->InsertElementBeforeElement(first_slot->element(),
                                         combine_element);
    Driver()->AddAttribute(combine_element, HtmlName::kSrc,
                           partition->url());
  }

  // Make a script element with eval(<variable name>), and replace
  // the existing element with it.
  void MakeScriptElement(int slot_index) {
    HtmlResourceSlot* html_slot = static_cast<HtmlResourceSlot*>(
        slot(slot_index).get());
    // Create a new element that doesn't have any children the
    // original element had.
    HtmlElement* original = html_slot->element();
    HtmlElement* element = Driver()->NewElement(NULL, HtmlName::kScript);
    Driver()->InsertElementBeforeElement(original, element);
    //    Driver()->DeleteElement(original);
    GoogleString var_name = filter_->VarName(
        html_slot->resource()->url());
    HtmlNode* script_code = Driver()->NewCharactersNode(
        element, StrCat("eval(", var_name, ");"));
    Driver()->AppendChild(element, script_code);
    // Don't render this slot because it no longer has a resource attached
    // to it.
    //    html_slot->set_disable_rendering(true);
    html_slot->set_should_delete_element(true);
  }

  JsCombineFilter::JsCombiner combiner_;
  JsCombineFilter* filter_;
  std::vector<HtmlElement*> elements_;
  bool fresh_combination_;
  UrlMultipartEncoder encoder_;
};

bool JsCombineFilter::JsCombiner::WritePiece(
    int index, const Resource* input, OutputResource* combination,
    Writer* writer, MessageHandler* handler) {
  // We write out code of each script into a variable.
  writer->Write(StrCat("var ", filter_->VarName(input->url()), " = "),
                handler);

  StringPiece original = input->contents();
  GoogleString escaped;
  JavascriptCodeBlock::ToJsStringLiteral(original, &escaped);

  writer->Write(escaped, handler);
  writer->Write(";\n", handler);
  return true;
}

JsCombineFilter::JsCombineFilter(RewriteDriver* driver)
    : RewriteFilter(driver),
      script_scanner_(driver),
      script_depth_(0),
      current_js_script_(NULL),
      context_(MakeContext()) {
}

JsCombineFilter::~JsCombineFilter() {
}

void JsCombineFilter::Initialize(Statistics* statistics) {
  statistics->AddVariable(kJsFileCountReduction);
}

void JsCombineFilter::StartDocumentImpl() {
}

void JsCombineFilter::StartElementImpl(HtmlElement* element) {
  HtmlElement::Attribute* src = NULL;
  ScriptTagScanner::ScriptClassification classification =
      script_scanner_.ParseScriptElement(element, &src);
  switch (classification) {
    case ScriptTagScanner::kNonScript:
      if (script_depth_ > 0) {
        // We somehow got some tag inside a script. Be conservative ---
        // it may be meaningful so we don't want to destroy it;
        // so flush the complete things before us, and call it a day.
        if (IsCurrentScriptInCombination()) {
          context_->RemoveLastElement();
        }
        NextCombination();
      }
      break;

    case ScriptTagScanner::kJavaScript:
      ConsiderJsForCombination(element, src);
      ++script_depth_;
      break;

    case ScriptTagScanner::kUnknownScript:
      // We have something like vbscript. Handle this as a barrier
      NextCombination();
      ++script_depth_;
      break;
  }
}

void JsCombineFilter::EndElementImpl(HtmlElement* element) {
  if (element->keyword() == HtmlName::kScript) {
    --script_depth_;
    if (script_depth_ == 0) {
      current_js_script_ = NULL;
    }
  }
}

void JsCombineFilter::IEDirective(HtmlIEDirectiveNode* directive) {
  NextCombination();
}

void JsCombineFilter::Characters(HtmlCharactersNode* characters) {
  // If a script has non-whitespace data inside of it, we cannot
  // replace its contents with a call to eval, as they may be needed.
  if (script_depth_ > 0 && !OnlyWhitespace(characters->contents())) {
    if (IsCurrentScriptInCombination()) {
      context_->RemoveLastElement();
      NextCombination();
    }
  }
}

void JsCombineFilter::Flush() {
  // We try to combine what we have thus far the moment we see a flush.
  // This serves two purposes:
  // 1) Let's us edit elements while they are still rewritable,
  //    but as late as possible.
  // 2) Ensures we do combine eventually (as we will get a flush at the end of
  //    parsing).
  NextCombination();
}

// Determine if we can add this script to the combination or not.
// If not, call NextCombination() to write out what we've got and then
// reset.
void JsCombineFilter::ConsiderJsForCombination(HtmlElement* element,
                                               HtmlElement::Attribute* src) {
  // Worst-case scenario is if we somehow ended up with nested scripts.
  // In this case, we just give up entirely.
  if (script_depth_ > 0) {
    driver_->WarningHere("Nested <script> elements");
    context_->Reset();
    return;
  }

  // Opening a new script normally...
  current_js_script_ = element;

  // Now we may have something that's not combinable; in those cases we would
  // like to flush as much as possible.
  // TODO(morlovich): if we stick with the current eval-based strategy, this
  // is way too conservative, as we keep multiple script elements for
  // actual execution.

  // If our current script may be inside a noscript, which means
  // we should not be making it runnable.
  if (noscript_element() != NULL) {
    NextCombination();
    return;
  }

  // An inline script.
  if (src == NULL || src->DecodedValueOrNull() == NULL) {
    NextCombination();
    return;
  }

  // We do not try to merge in a <script with async/defer> or for/event.
  // TODO(morlovich): is it worth combining multiple scripts with
  // async/defer if the flags are the same?
  if (script_scanner_.ExecutionMode(element) != script_scanner_.kExecuteSync) {
    NextCombination();
    return;
  }

  // Now we see if policy permits us merging this element with previous ones.
  context_->AddElement(element, src);
}

bool JsCombineFilter::IsCurrentScriptInCombination() const {
  return context_->HasElementLast(current_js_script_);
}

GoogleString JsCombineFilter::VarName(const GoogleString& url) const {
  // We hash the non-host portion of URL to keep it consistent when sharding.
  // This is safe since we never include URLs from different hosts in a single
  // combination.
  GoogleString url_hash = JavascriptCodeBlock::JsUrlHash(url,
    resource_manager_->hasher());

  return StrCat("mod_pagespeed_", url_hash);
}

JsCombineFilter::Context* JsCombineFilter::MakeContext() {
  return new Context(driver_, this);
}

RewriteContext* JsCombineFilter::MakeRewriteContext() {
  return MakeContext();
}

JsCombineFilter::JsCombiner* JsCombineFilter::combiner() const {
  return context_->combiner();
}

// In async flow, tell the rewrite_driver to write out the last
// combination, and reset our context to a new one.
// In sync flow, just write out what we have so far, and then
// reset the context.
void JsCombineFilter::NextCombination() {
  if (!context_->empty()) {
    driver_->InitiateRewrite(context_.release());
    context_.reset(MakeContext());
  }
  context_->Reset();
}

}  // namespace net_instaweb
