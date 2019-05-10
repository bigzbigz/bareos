/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2000-2012 Free Software Foundation Europe e.V.
   Copyright (C) 2011-2012 Planets Communications B.V.
   Copyright (C) 2013-2019 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/

#include "lib/parse_conf_state_machine.h"
#include "lib/parse_conf.h"
#include "lib/resource_item.h"
#include "lib/lex.h"

ConfigParserStateMachine::ConfigParserStateMachine(
    const char* config_file_name,
    void* caller_ctx,
    LEX_ERROR_HANDLER* scan_error,
    LEX_WARNING_HANDLER* scan_warning,
    ConfigurationParser& my_config)
    : config_file_name_(config_file_name)
    , caller_ctx_(caller_ctx)
    , scan_error_(scan_error)
    , scan_warning_(scan_warning)
    , my_config_(my_config)
{
  return;
};

ConfigParserStateMachine::~ConfigParserStateMachine()
{
  while (lexical_parser_) { lexical_parser_ = LexCloseFile(lexical_parser_); }
}

bool ConfigParserStateMachine::ParseAllTokens()
{
  int token;

  while ((token = LexGetToken(lexical_parser_, BCT_ALL)) != BCT_EOF) {
    Dmsg3(900, "parse state=%d parser_pass_number_=%d got token=%s\n", state,
          parser_pass_number_, lex_tok_to_str(token));
    switch (state) {
      case ParseState::kInit:
        switch (ParserInitResource(token)) {
          case ParseInternalReturnCode::kGetNextToken:
          case ParseInternalReturnCode::kNextState:
            continue;
          case ParseInternalReturnCode::kError:
            return false;
          default:
            ASSERT(false);
        }
        break;
      case ParseState::kResource:
        switch (ScanResource(token)) {
          case ParseInternalReturnCode::kGetNextToken:
            continue;
          case ParseInternalReturnCode::kError:
            return false;
          default:
            ASSERT(false);
        }
        break;
      default:
        scan_err1(lexical_parser_, _("Unknown parser state %d\n"), state);
        return false;
    }
  }
  return true;
}

ConfigParserStateMachine::ParseInternalReturnCode
ConfigParserStateMachine::ScanResource(int token)
{
  switch (token) {
    case BCT_BOB:
      config_level_++;
      return ParseInternalReturnCode::kGetNextToken;
    case BCT_IDENTIFIER: {
      if (config_level_ != 1) {
        scan_err1(lexical_parser_, _("not in resource definition: %s"),
                  lexical_parser_->str);
        return ParseInternalReturnCode::kError;
      }

      int resource_item_index = my_config_.GetResourceItemIndex(
          resource_memory_.resource_items_, lexical_parser_->str);

      if (resource_item_index >= 0) {
        ResourceItem* item = nullptr;
        item = &resource_memory_.resource_items_[resource_item_index];
        if (!(item->flags & CFG_ITEM_NO_EQUALS)) {
          token = LexGetToken(lexical_parser_, BCT_SKIP_EOL);
          Dmsg1(900, "in BCT_IDENT got token=%s\n", lex_tok_to_str(token));
          if (token != BCT_EQUALS) {
            scan_err1(lexical_parser_, _("expected an equals, got: %s"),
                      lexical_parser_->str);
            return ParseInternalReturnCode::kError;
          }
        }

        if (item->flags & CFG_ITEM_DEPRECATED) {
          scan_warn2(lexical_parser_,
                     _("using deprecated keyword %s on line %d"), item->name,
                     lexical_parser_->line_no);
          // only warning
        }

        Dmsg1(800, "calling handler for %s\n", item->name);

        if (!my_config_.StoreResource(item->type, lexical_parser_, item,
                                      resource_item_index,
                                      parser_pass_number_)) {
          if (my_config_.store_res_) {
            my_config_.store_res_(lexical_parser_, item, resource_item_index,
                                  parser_pass_number_);
          }
        }
      } else {
        Dmsg2(900, "config_level_=%d id=%s\n", config_level_,
              lexical_parser_->str);
        Dmsg1(900, "Keyword = %s\n", lexical_parser_->str);
        scan_err1(lexical_parser_,
                  _("Keyword \"%s\" not permitted in this resource.\n"
                    "Perhaps you left the trailing brace off of the "
                    "previous resource."),
                  lexical_parser_->str);
        return ParseInternalReturnCode::kError;
      }
      return ParseInternalReturnCode::kGetNextToken;
    }
    case BCT_EOB:
      config_level_--;
      state = ParseState::kInit;
      Dmsg0(900, "BCT_EOB => define new resource\n");
      if (!resource_memory_.static_resource_->resource_name_) {
        scan_err0(lexical_parser_, _("Name not specified for resource"));
        return ParseInternalReturnCode::kError;
      }
      /* save resource */
      if (!my_config_.SaveResourceCb_(resource_memory_.rcode_,
                                      resource_memory_.resource_items_,
                                      parser_pass_number_)) {
        scan_err0(lexical_parser_, _("SaveResource failed"));
        return ParseInternalReturnCode::kError;
      }
      if (parser_pass_number_ == 2) {
        // free all resource memory from second pass
        if (resource_memory_.static_resource_) {
          if (resource_memory_.static_resource_->resource_name_) {
            delete resource_memory_.static_resource_->resource_name_;
          }
          delete resource_memory_.static_resource_;
        }
      }
      return ParseInternalReturnCode::kGetNextToken;

    case BCT_EOL:
      return ParseInternalReturnCode::kGetNextToken;

    default:
      scan_err2(lexical_parser_,
                _("unexpected token %d %s in resource definition"), token,
                lex_tok_to_str(token));
      return ParseInternalReturnCode::kError;
  }
  return ParseInternalReturnCode::kGetNextToken;
}

ConfigParserStateMachine::ParseInternalReturnCode
ConfigParserStateMachine::ParserInitResource(int token)
{
  const char* resource_identifier = lexical_parser_->str;

  switch (token) {
    case BCT_EOL:
    case BCT_UTF8_BOM:
      return ParseInternalReturnCode::kGetNextToken;
    case BCT_UTF16_BOM:
      scan_err0(lexical_parser_,
                _("Currently we cannot handle UTF-16 source files. "
                  "Please convert the conf file to UTF-8\n"));
      return ParseInternalReturnCode::kError;
    default:
      if (token != BCT_IDENTIFIER) {
        scan_err1(lexical_parser_,
                  _("Expected a Resource name identifier, got: %s"),
                  resource_identifier);
        return ParseInternalReturnCode::kError;
      }
      break;
  }

  ResourceTable* resource_table;
  resource_table = my_config_.GetResourceTable(resource_identifier);

  bool init_resource_memory_ok = false;

  if (resource_table && resource_table->items) {
    resource_memory_.resource_items_ = resource_table->items;
    resource_memory_.rcode_ = resource_table->rcode;

    my_config_.InitResource(
        resource_memory_.rcode_, resource_memory_.resource_items_,
        parser_pass_number_, resource_table->ResourceSpecificInitializer);

    ASSERT(resource_table->static_resource_);
    resource_memory_.static_resource_ = *resource_table->static_resource_;
    ASSERT(resource_memory_.static_resource_);

    state = ParseState::kResource;

    init_resource_memory_ok = true;
  }

  if (!init_resource_memory_ok) {
    scan_err1(lexical_parser_, _("expected resource identifier, got: %s"),
              resource_identifier);
    return ParseInternalReturnCode::kError;
  }
  return ParseInternalReturnCode::kNextState;
}

bool ConfigParserStateMachine::InitParserPass()
{
  parser_pass_number_++;
  ASSERT(parser_pass_number_ < 3);

  // close files from the pass before
  while (lexical_parser_) { lexical_parser_ = LexCloseFile(lexical_parser_); }

  Dmsg1(900, "ParseConfig parser_pass_number_ %d\n", parser_pass_number_);
  if ((lexical_parser_ =
           lex_open_file(lexical_parser_, config_file_name_.c_str(),
                         scan_error_, scan_warning_)) == nullptr) {
    my_config_.lex_error(config_file_name_.c_str(), scan_error_, scan_warning_);
    return false;
  }
  LexSetErrorHandlerErrorType(lexical_parser_, my_config_.err_type_);
  lexical_parser_->error_counter = 0;
  lexical_parser_->caller_ctx = caller_ctx_;
  return true;
}

void ConfigParserStateMachine::DumpResourcesAfterSecondPass()
{
  if (debug_level >= 900 && parser_pass_number_ == 2) {
    for (int i = my_config_.r_first_; i <= my_config_.r_last_; i++) {
      my_config_.DumpResourceCb_(i,
                                 my_config_.res_head_[i - my_config_.r_first_],
                                 PrintMessage, nullptr, false, false);
    }
  }
}

ConfigParserStateMachine::ParserError ConfigParserStateMachine::GetParseError()
    const
{
  if (state != ParseState::kInit) {
    return ParserError::kResourceIncomplete;
  } else if (lexical_parser_->error_counter > 0) {
    return ParserError::kParserError;
  } else {
    return ParserError::kNoError;
  }
}