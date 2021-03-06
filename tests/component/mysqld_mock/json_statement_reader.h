/*
  Copyright (c) 2017, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifndef MYSQLD_MOCK_JSON_STATEMENT_READER_INCLUDED
#define MYSQLD_MOCK_JSON_STATEMENT_READER_INCLUDED

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "mysql_protocol_common.h"

namespace server_mock {


struct Response {
  virtual ~Response() {};
};

/** @brief Keeps result data for single SQL statement that returns
 *         resultset.
 **/
struct ResultsetResponse : public Response {
  std::vector<column_info_type> columns;
  std::vector<row_values_type> rows;
};

struct OkResponse : public Response {
  OkResponse(unsigned int last_insert_id_=0, unsigned int warning_count_=0) : last_insert_id(last_insert_id_), warning_count(warning_count_) {}

  unsigned int last_insert_id;
  unsigned int warning_count;
};

struct ErrorResponse : public Response {
  ErrorResponse(unsigned int code_, std::string msg_, std::string sql_state_="HY000") : code(code_), msg(msg_), sql_state(sql_state_) {}

  unsigned int code;
  std::string msg;
  std::string sql_state;
};


/** @class StatementAndResponse
 *
 * @brief Keeps single SQL statement data.
 **/
struct StatementAndResponse {
  /** @enum statement_response_type
   *
   * Response expected for given SQL statement.
   **/
  enum class statement_response_type {
     STMT_RES_OK, STMT_RES_ERROR, STMT_RES_RESULT
  };

  // true if statement is a regex
  bool statement_is_regex{false};
  // SQL statement
  std::string statement;
  // exected response type for the statement
  statement_response_type response_type;

  std::unique_ptr<Response> response;

  // execution time in microseconds
  std::chrono::microseconds exec_time{0};
};

/** @class QueriesJsonReader
 *
 * @brief  Responsible for reading the json file with
 *         the expected statements and simplifying the data
 *         structures used by RapidJSON into vectors.
 **/
class QueriesJsonReader {
 public:

  /** @brief Constructor.
   *
   * @param filename Path to the json file with definitins
   *         of the expected SQL statements and responses
   **/
  QueriesJsonReader(const std::string &filename);

  /** @brief Returns the data about the next statement from the
   *         json file. If there is no more statements it returns
   *         empty statement.
   **/
  StatementAndResponse get_next_statement();

  /** @brief Returns the default execution time in microseconds. If
   *         no default execution time is provided in json file, then
   *         0 microseconds is returned.
   **/
  std::chrono::microseconds get_default_exec_time();

  ~QueriesJsonReader();
private:
  // This is to avoid including RapidJSON headers here, which would cause
  // them included also in other files (they give tons of warnings, which
  // better suppres only for single implementation file).
  struct Pimpl;
  std::unique_ptr<Pimpl> pimpl_;
};


} // namespace

#endif // MYSQLD_MOCK_JSON_STATEMENT_READER_INCLUDED
