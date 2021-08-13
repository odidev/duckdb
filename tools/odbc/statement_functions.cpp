#include "statement_functions.hpp"
#include "odbc_interval.hpp"
#include "odbc_fetch.hpp"

#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/operator/decimal_cast_operators.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"

#include <algorithm>

using std::string;

using duckdb::date_t;
using duckdb::Decimal;
using duckdb::DecimalType;
using duckdb::dtime_t;
using duckdb::hugeint_t;
using duckdb::interval_t;
using duckdb::LogicalType;
using duckdb::LogicalTypeId;
using duckdb::OdbcInterval;
using duckdb::Store;
using duckdb::string_t;
using duckdb::timestamp_t;

SQLRETURN duckdb::PrepareStmt(SQLHSTMT statement_handle, SQLCHAR *statement_text, SQLINTEGER text_length) {
	return duckdb::WithStatement(statement_handle, [&](duckdb::OdbcHandleStmt *stmt) {
		if (stmt->stmt) {
			stmt->stmt.reset();
		}
		if (stmt->res) {
			stmt->res.reset();
		}
		stmt->odbc_fetcher->ClearChunks();
		// we should not clear the parameters because of SQLExecDirect may reuse them
		// stmt->params.resize(0);
		// we should not clear the bound columns because SQLBindCol might bind columns in it
		// stmt->bound_cols.resize(0);

		auto query = duckdb::OdbcUtils::ReadString(statement_text, text_length);
		stmt->stmt = stmt->dbc->conn->Prepare(query);
		if (!stmt->stmt->success) {
			stmt->error_messages.emplace_back(stmt->stmt->error);
			return SQL_ERROR;
		}
		stmt->params.resize(stmt->stmt->n_param);
		stmt->bound_cols.resize(stmt->stmt->ColumnCount());
		return SQL_SUCCESS;
	});
}

SQLRETURN duckdb::ExecuteStmt(SQLHSTMT statement_handle) {
	return duckdb::WithStatement(statement_handle, [&](duckdb::OdbcHandleStmt *stmt) {
		if (stmt->res) {
			stmt->res.reset();
		}
		stmt->odbc_fetcher->ClearChunks();

		stmt->open = false;
		if (stmt->rows_fetched_ptr) {
			*stmt->rows_fetched_ptr = 0;
		}
		stmt->res = stmt->stmt->Execute(stmt->params);
		if (!stmt->res->success) {
			stmt->error_messages.emplace_back(stmt->res->error);
			return SQL_ERROR;
		}
		stmt->open = true;
		return SQL_SUCCESS;
	});
}

SQLRETURN duckdb::FetchStmtResult(SQLHSTMT statement_handle, SQLSMALLINT fetch_orientation, SQLLEN fetch_offset) {
	return duckdb::WithStatementResult(statement_handle, [&](duckdb::OdbcHandleStmt *stmt) -> SQLRETURN {
		if (!stmt->open) {
			return SQL_NO_DATA;
		}
		SQLRETURN ret = stmt->odbc_fetcher->Fetch(statement_handle, stmt, fetch_orientation, fetch_offset);
		if (!SQL_SUCCEEDED(ret)) {
			return ret;
		}

		stmt->odbc_fetcher->AssertCurrentChunk();
		return SQL_SUCCESS;
	});
}

//! Static fuctions used by GetDataStmtResult //

static bool ValidateType(LogicalTypeId input, LogicalTypeId expected, duckdb::OdbcHandleStmt *stmt) {
	if (input != expected) {
		stmt->error_messages.emplace_back("Type mismatch error: received " + LogicalTypeIdToString(input) +
		                                  ", but expected " + LogicalTypeIdToString(expected));
		return false;
	}
	return true;
}

static void LogInvalidCast(const LogicalType &from_type, const LogicalType &to_type, duckdb::OdbcHandleStmt *stmt) {
	string msg = "Not implemented Error: Unimplemented type for cast (" + from_type.ToString() + " -> " +
	             to_type.ToString() + ")";
	stmt->error_messages.emplace_back(msg);
}

template <class SRC, class DEST = SRC>
static SQLRETURN GetInternalValue(duckdb::OdbcHandleStmt *stmt, const duckdb::Value &val, const LogicalType &type,
                                  SQLPOINTER target_value_ptr, SQLLEN *str_len_or_ind_ptr) {
	// https://docs.microsoft.com/en-us/sql/odbc/reference/syntax/sqlbindcol-function
	// When the driver returns fixed-length data, such as an integer or a date structure, the driver ignores
	// BufferLength... D_ASSERT(((size_t)buffer_length) >= sizeof(DEST));
	try {
		auto casted_value = val.CastAs(type).GetValue<SRC>();
		Store<DEST>(casted_value, (duckdb::data_ptr_t)target_value_ptr);
		if (str_len_or_ind_ptr) {
			*str_len_or_ind_ptr = sizeof(casted_value);
		}
		return SQL_SUCCESS;
	} catch (std::exception &ex) {
		stmt->error_messages.emplace_back(ex.what());
		return SQL_ERROR;
	}
}

template <class CAST_OP, typename TARGET_TYPE, class CAST_FUNC = std::function<timestamp_t(int64_t)>>
static bool CastTimestampValue(duckdb::OdbcHandleStmt *stmt, const duckdb::Value &val, TARGET_TYPE &target,
                               CAST_FUNC cast_timestamp_fun) {
	try {
		timestamp_t timestamp = cast_timestamp_fun(val.GetValue<int64_t>());
		target = CAST_OP::template Operation<timestamp_t, TARGET_TYPE>(timestamp);
		return true;
	} catch (duckdb::Exception &ex) {
		stmt->error_messages.emplace_back(ex.what());
		return false;
	}
}

SQLRETURN duckdb::GetDataStmtResult(SQLHSTMT statement_handle, SQLUSMALLINT col_or_param_num, SQLSMALLINT target_type,
                                    SQLPOINTER target_value_ptr, SQLLEN buffer_length, SQLLEN *str_len_or_ind_ptr) {

	return duckdb::WithStatementResult(statement_handle, [&](duckdb::OdbcHandleStmt *stmt) -> SQLRETURN {
		if (!target_value_ptr) {
			return SQL_ERROR;
		}

		Value val;
		stmt->odbc_fetcher->GetValue(col_or_param_num - 1, val);
		if (val.is_null) {
			if (!str_len_or_ind_ptr) {
				return SQL_ERROR;
			}
			*str_len_or_ind_ptr = SQL_NULL_DATA;
			return SQL_SUCCESS;
		}

		switch (target_type) {
		case SQL_C_SSHORT:
			return GetInternalValue<int16_t, SQLSMALLINT>(stmt, val, LogicalType::SMALLINT, target_value_ptr,
			                                              str_len_or_ind_ptr);
		case SQL_C_USHORT:
			return GetInternalValue<uint16_t, SQLUSMALLINT>(stmt, val, LogicalType::USMALLINT, target_value_ptr,
			                                                str_len_or_ind_ptr);
		case SQL_C_LONG:
		case SQL_C_SLONG:
			return GetInternalValue<int32_t, SQLINTEGER>(stmt, val, LogicalType::INTEGER, target_value_ptr,
			                                             str_len_or_ind_ptr);
		case SQL_C_ULONG:
			return GetInternalValue<uint32_t, SQLUINTEGER>(stmt, val, LogicalType::UINTEGER, target_value_ptr,
			                                               str_len_or_ind_ptr);
		case SQL_C_FLOAT:
			return GetInternalValue<float, SQLREAL>(stmt, val, LogicalType::FLOAT, target_value_ptr,
			                                        str_len_or_ind_ptr);
		case SQL_C_DOUBLE:
			return GetInternalValue<double, SQLFLOAT>(stmt, val, LogicalType::DOUBLE, target_value_ptr,
			                                          str_len_or_ind_ptr);
		case SQL_C_BIT: {
			LogicalType char_type = LogicalType(LogicalTypeId::CHAR);
			return GetInternalValue<SQLCHAR>(stmt, val, char_type, target_value_ptr, str_len_or_ind_ptr);
		}
		case SQL_C_STINYINT:
			return GetInternalValue<int8_t, SQLSCHAR>(stmt, val, LogicalType::TINYINT, target_value_ptr,
			                                          str_len_or_ind_ptr);
		case SQL_C_UTINYINT:
			return GetInternalValue<uint8_t, uint8_t>(stmt, val, LogicalType::UTINYINT, target_value_ptr,
			                                          str_len_or_ind_ptr);
		case SQL_C_SBIGINT:
			return GetInternalValue<int64_t, SQLBIGINT>(stmt, val, LogicalType::BIGINT, target_value_ptr,
			                                            str_len_or_ind_ptr);
		case SQL_C_UBIGINT:
			// case SQL_C_BOOKMARK: // same ODBC type (\\TODO we don't support bookmark types)
			return GetInternalValue<uint64_t, SQLUBIGINT>(stmt, val, LogicalType::UBIGINT, target_value_ptr,
			                                              str_len_or_ind_ptr);
		case SQL_C_WCHAR: {
			std::string str = val.GetValue<std::string>();
			std::wstring w_str = std::wstring(str.begin(), str.end());
			auto out_len = swprintf((wchar_t *)target_value_ptr, buffer_length, L"%ls", w_str.c_str());

			if (str_len_or_ind_ptr) {
				*str_len_or_ind_ptr = out_len;
			}
			return SQL_SUCCESS;
		}
		// case SQL_C_VARBOOKMARK: // same ODBC type (\\TODO we don't support bookmark types)
		case SQL_C_BINARY: {
			// threating binary values as BLOB type
			string blob = duckdb::Blob::ToBlob(duckdb::string_t(val.GetValue<string>().c_str()));
			auto out_len = snprintf((char *)target_value_ptr, buffer_length, "%s", blob.c_str());

			if (str_len_or_ind_ptr) {
				*str_len_or_ind_ptr = out_len;
			}
			return SQL_SUCCESS;
		}
		case SQL_C_CHAR: {
			auto out_len = snprintf((char *)target_value_ptr, buffer_length, "%s", val.GetValue<string>().c_str());

			if (str_len_or_ind_ptr) {
				*str_len_or_ind_ptr = out_len;
			}
			return SQL_SUCCESS;
		}
		case SQL_C_NUMERIC: {
			if (!ValidateType(val.type().id(), LogicalTypeId::DECIMAL, stmt)) {
				return SQL_ERROR;
			}
			SQL_NUMERIC_STRUCT *numeric = (SQL_NUMERIC_STRUCT *)target_value_ptr;
			auto dataptr = (duckdb::data_ptr_t)numeric->val;
			// reset numeric val to remove some garbage
			memset(dataptr, '\0', SQL_MAX_NUMERIC_LEN);

			numeric->sign = 1;
			numeric->precision = numeric->scale = 0;

			string str_val = val.ToString();
			auto width = str_val.size();

			if (str_val[0] == '-') {
				numeric->sign = 0;
				str_val.erase(std::remove(str_val.begin(), str_val.end(), '-'), str_val.end());
				// uncounting negative signal '-'
				--width;
			}

			auto pos_dot = str_val.find('.');
			if (pos_dot != string::npos) {
				str_val.erase(std::remove(str_val.begin(), str_val.end(), '.'), str_val.end());
				numeric->scale = str_val.size() - pos_dot;

				string str_fraction = str_val.substr(pos_dot);
				// case all digits in fraction is 0, remove them
				if (std::stoi(str_fraction) == 0) {
					str_val.erase(str_val.begin() + pos_dot, str_val.end());
				}
				width = str_val.size();
			}
			numeric->precision = width;

			string_t str_t(str_val.c_str(), width);
			if (numeric->precision <= Decimal::MAX_WIDTH_INT64) {
				int64_t val_i64;
				if (!duckdb::TryCast::Operation(str_t, val_i64)) {
					return SQL_ERROR;
				}
				memcpy(dataptr, &val_i64, sizeof(val_i64));
			} else {
				hugeint_t huge_int;
				string error_message;
				if (!duckdb::TryCastToDecimal::Operation<string_t, hugeint_t>(str_t, huge_int, &error_message,
				                                                              numeric->precision, numeric->scale)) {
					return SQL_ERROR;
				}
				memcpy(dataptr, &huge_int.lower, sizeof(huge_int.lower));
				memcpy(dataptr + sizeof(huge_int.lower), &huge_int.upper, sizeof(huge_int.upper));
			}

			if (str_len_or_ind_ptr) {
				*str_len_or_ind_ptr = sizeof(SQL_NUMERIC_STRUCT);
			}
			return SQL_SUCCESS;
		}
		case SQL_C_TYPE_DATE: {
			date_t date;
			switch (val.type().id()) {
			case LogicalTypeId::DATE:
				date = val.GetValue<date_t>();
				break;
			case LogicalTypeId::TIMESTAMP_SEC: {
				if (!CastTimestampValue<duckdb::Cast, date_t>(stmt, val, date, duckdb::Timestamp::FromEpochSeconds)) {
					return SQL_ERROR;
				}
				break;
			}
			case LogicalTypeId::TIMESTAMP_MS: {
				if (!CastTimestampValue<duckdb::Cast, date_t>(stmt, val, date, duckdb::Timestamp::FromEpochMs)) {
					return SQL_ERROR;
				}
				break;
			}
			case LogicalTypeId::TIMESTAMP: {
				if (!CastTimestampValue<duckdb::Cast, date_t>(stmt, val, date,
				                                              duckdb::Timestamp::FromEpochMicroSeconds)) {
					return SQL_ERROR;
				}
				break;
			}
			case LogicalTypeId::TIMESTAMP_NS: {
				if (!CastTimestampValue<duckdb::Cast, date_t>(stmt, val, date,
				                                              duckdb::Timestamp::FromEpochNanoSeconds)) {
					return SQL_ERROR;
				}
				break;
			}
			case LogicalTypeId::VARCHAR: {
				string val_str = val.GetValue<string>();
				auto str_input = string_t(val_str);
				if (!TryCast::Operation<string_t, date_t>(str_input, date)) {
					stmt->error_messages.emplace_back(CastExceptionText<string_t, date_t>(str_input));
					return SQL_ERROR;
				}
				break;
			}
			default:
				LogInvalidCast(val.type(), LogicalType::DATE, stmt);
				return SQL_ERROR;
			} // end switch "val.type().id()": SQL_C_TYPE_DATE

			SQL_DATE_STRUCT *date_struct = (SQL_DATE_STRUCT *)target_value_ptr;
			int32_t year, month, day;
			Date::Convert(date, year, month, day);
			date_struct->year = year;
			date_struct->month = month;
			date_struct->day = day;
			if (str_len_or_ind_ptr) {
				*str_len_or_ind_ptr = sizeof(SQL_DATE_STRUCT);
			}
			return SQL_SUCCESS;
		}
		case SQL_C_TYPE_TIME: {
			dtime_t time;
			switch (val.type().id()) {
			case LogicalTypeId::TIME:
				time = val.GetValue<dtime_t>();
				break;
			case LogicalTypeId::TIMESTAMP_SEC: {
				if (!CastTimestampValue<duckdb::Cast, dtime_t>(stmt, val, time, duckdb::Timestamp::FromEpochSeconds)) {
					return SQL_ERROR;
				}
				break;
			}
			case LogicalTypeId::TIMESTAMP_MS: {
				if (!CastTimestampValue<duckdb::Cast, dtime_t>(stmt, val, time, duckdb::Timestamp::FromEpochMs)) {
					return SQL_ERROR;
				}
				break;
			}
			case LogicalTypeId::TIMESTAMP: {
				if (!CastTimestampValue<duckdb::Cast, dtime_t>(stmt, val, time,
				                                               duckdb::Timestamp::FromEpochMicroSeconds)) {
					return SQL_ERROR;
				}
				break;
			}
			case LogicalTypeId::TIMESTAMP_NS: {
				if (!CastTimestampValue<duckdb::Cast, dtime_t>(stmt, val, time,
				                                               duckdb::Timestamp::FromEpochNanoSeconds)) {
					return SQL_ERROR;
				}
				break;
			}
			case LogicalTypeId::VARCHAR: {
				string val_str = val.GetValue<string>();
				auto str_input = string_t(val_str);
				if (!TryCast::Operation<string_t, dtime_t>(str_input, time)) {
					stmt->error_messages.emplace_back(CastExceptionText<string_t, dtime_t>(str_input));
					return SQL_ERROR;
				}
				break;
			}
			default:
				LogInvalidCast(val.type(), LogicalType::TIME, stmt);
				return SQL_ERROR;
			} // end switch "val.type().id()": SQL_C_TYPE_TIME

			SQL_TIME_STRUCT *time_struct = (SQL_TIME_STRUCT *)target_value_ptr;
			int32_t hour, minute, second, micros;
			duckdb::Time::Convert(time, hour, minute, second, micros);

			time_struct->hour = hour;
			time_struct->minute = minute;
			time_struct->second = second;
			if (str_len_or_ind_ptr) {
				*str_len_or_ind_ptr = sizeof(SQL_TIME_STRUCT);
			}
			return SQL_SUCCESS;
		}
		case SQL_C_TYPE_TIMESTAMP: {
			timestamp_t timestamp;
			switch (val.type().id()) {
			case LogicalTypeId::TIMESTAMP_SEC:
				timestamp = duckdb::Timestamp::FromEpochSeconds(val.GetValue<int64_t>());
				break;
			case LogicalTypeId::TIMESTAMP_MS:
				timestamp = duckdb::Timestamp::FromEpochMs(val.GetValue<int64_t>());
				break;
			case LogicalTypeId::TIMESTAMP:
				timestamp = duckdb::Timestamp::FromEpochMicroSeconds(val.GetValue<int64_t>());
				break;
			case LogicalTypeId::TIMESTAMP_NS:
				timestamp = duckdb::Timestamp::FromEpochNanoSeconds(val.GetValue<int64_t>());
				break;
			case LogicalTypeId::DATE: {
				auto date_input = val.GetValue<date_t>();
				if (!TryCast::Operation<date_t, timestamp_t>(date_input, timestamp)) {
					stmt->error_messages.emplace_back(CastExceptionText<date_t, timestamp_t>(date_input));
					return SQL_ERROR;
				}
				break;
			}
			case LogicalTypeId::VARCHAR: {
				string val_str = val.GetValue<string>();
				auto str_input = string_t(val_str);
				if (!TryCast::Operation<string_t, timestamp_t>(str_input, timestamp)) {
					stmt->error_messages.emplace_back(CastExceptionText<string_t, timestamp_t>(str_input));
					return SQL_ERROR;
				}
				break;
			}
			default:
				LogInvalidCast(val.type(), LogicalType::TIMESTAMP, stmt);
				return SQL_ERROR;
			} // end switch "val.type().id()"

			SQL_TIMESTAMP_STRUCT *timestamp_struct = (SQL_TIMESTAMP_STRUCT *)target_value_ptr;
			date_t date = duckdb::Timestamp::GetDate(timestamp);

			int32_t year, month, day;
			Date::Convert(date, year, month, day);
			timestamp_struct->year = year;
			timestamp_struct->month = month;
			timestamp_struct->day = day;

			dtime_t time = duckdb::Timestamp::GetTime(timestamp);
			int32_t hour, minute, second, micros;
			duckdb::Time::Convert(time, hour, minute, second, micros);
			timestamp_struct->hour = hour;
			timestamp_struct->minute = minute;
			timestamp_struct->second = second;
			timestamp_struct->fraction = micros;
			if (str_len_or_ind_ptr) {
				*str_len_or_ind_ptr = sizeof(SQL_TIMESTAMP_STRUCT);
			}

			return SQL_SUCCESS;
		}
		case SQL_C_INTERVAL_YEAR: {
			interval_t interval;
			if (!OdbcInterval::GetInterval(val, interval, stmt)) {
				LogInvalidCast(val.type(), LogicalType::INTERVAL, stmt);
				return SQL_ERROR;
			}

			SQL_INTERVAL_STRUCT *interval_struct = (SQL_INTERVAL_STRUCT *)target_value_ptr;
			OdbcInterval::SetYear(interval, interval_struct);
			OdbcInterval::SetSignal(interval, interval_struct);

			if (str_len_or_ind_ptr) {
				*str_len_or_ind_ptr = sizeof(SQL_INTERVAL_STRUCT);
			}

			return SQL_SUCCESS;
		}
		case SQL_C_INTERVAL_MONTH: {
			interval_t interval;
			if (!OdbcInterval::GetInterval(val, interval, stmt)) {
				LogInvalidCast(val.type(), LogicalType::INTERVAL, stmt);
				return SQL_ERROR;
			}

			SQL_INTERVAL_STRUCT *interval_struct = (SQL_INTERVAL_STRUCT *)target_value_ptr;
			OdbcInterval::SetMonth(interval, interval_struct);
			OdbcInterval::SetSignal(interval, interval_struct);

			if (str_len_or_ind_ptr) {
				*str_len_or_ind_ptr = sizeof(SQL_INTERVAL_STRUCT);
			}

			return SQL_SUCCESS;
		}
		case SQL_C_INTERVAL_DAY: {
			interval_t interval;
			if (!OdbcInterval::GetInterval(val, interval, stmt)) {
				LogInvalidCast(val.type(), LogicalType::INTERVAL, stmt);
				return SQL_ERROR;
			}

			SQL_INTERVAL_STRUCT *interval_struct = (SQL_INTERVAL_STRUCT *)target_value_ptr;
			OdbcInterval::SetDay(interval, interval_struct);
			OdbcInterval::SetSignal(interval, interval_struct);

			if (str_len_or_ind_ptr) {
				*str_len_or_ind_ptr = sizeof(SQL_INTERVAL_STRUCT);
			}

			return SQL_SUCCESS;
		}
		case SQL_C_INTERVAL_HOUR: {
			interval_t interval;
			if (!OdbcInterval::GetInterval(val, interval, stmt)) {
				LogInvalidCast(val.type(), LogicalType::INTERVAL, stmt);
				return SQL_ERROR;
			}

			SQL_INTERVAL_STRUCT *interval_struct = (SQL_INTERVAL_STRUCT *)target_value_ptr;
			OdbcInterval::SetHour(interval, interval_struct);
			OdbcInterval::SetSignal(interval, interval_struct);

			if (str_len_or_ind_ptr) {
				*str_len_or_ind_ptr = sizeof(SQL_INTERVAL_STRUCT);
			}

			return SQL_SUCCESS;
		}
		case SQL_C_INTERVAL_MINUTE: {
			interval_t interval;
			if (!OdbcInterval::GetInterval(val, interval, stmt)) {
				LogInvalidCast(val.type(), LogicalType::INTERVAL, stmt);
				return SQL_ERROR;
			}

			SQL_INTERVAL_STRUCT *interval_struct = (SQL_INTERVAL_STRUCT *)target_value_ptr;
			OdbcInterval::SetMinute(interval, interval_struct);
			OdbcInterval::SetSignal(interval, interval_struct);

			if (str_len_or_ind_ptr) {
				*str_len_or_ind_ptr = sizeof(SQL_INTERVAL_STRUCT);
			}

			return SQL_SUCCESS;
		}
		case SQL_C_INTERVAL_SECOND: {
			interval_t interval;
			if (!OdbcInterval::GetInterval(val, interval, stmt)) {
				LogInvalidCast(val.type(), LogicalType::INTERVAL, stmt);
				return SQL_ERROR;
			}

			SQL_INTERVAL_STRUCT *interval_struct = (SQL_INTERVAL_STRUCT *)target_value_ptr;
			OdbcInterval::SetSecond(interval, interval_struct);
			OdbcInterval::SetSignal(interval, interval_struct);

			if (str_len_or_ind_ptr) {
				*str_len_or_ind_ptr = sizeof(SQL_INTERVAL_STRUCT);
			}

			return SQL_SUCCESS;
		}
		case SQL_C_INTERVAL_YEAR_TO_MONTH: {
			interval_t interval;
			if (!OdbcInterval::GetInterval(val, interval, stmt)) {
				LogInvalidCast(val.type(), LogicalType::INTERVAL, stmt);
				return SQL_ERROR;
			}

			SQL_INTERVAL_STRUCT *interval_struct = (SQL_INTERVAL_STRUCT *)target_value_ptr;
			OdbcInterval::SetYear(interval, interval_struct);
			// fraction of years stored as months
			interval_struct->intval.year_month.month = std::abs(interval.months) % duckdb::Interval::MONTHS_PER_YEAR;
			interval_struct->interval_type = SQLINTERVAL::SQL_IS_YEAR_TO_MONTH;
			OdbcInterval::SetSignal(interval, interval_struct);

			if (str_len_or_ind_ptr) {
				*str_len_or_ind_ptr = sizeof(SQL_INTERVAL_STRUCT);
			}

			return SQL_SUCCESS;
		}
		case SQL_C_INTERVAL_DAY_TO_HOUR: {
			interval_t interval;
			if (!OdbcInterval::GetInterval(val, interval, stmt)) {
				LogInvalidCast(val.type(), LogicalType::INTERVAL, stmt);
				return SQL_ERROR;
			}

			SQL_INTERVAL_STRUCT *interval_struct = (SQL_INTERVAL_STRUCT *)target_value_ptr;
			OdbcInterval::SetDayToHour(interval, interval_struct);
			OdbcInterval::SetSignal(interval, interval_struct);

			if (str_len_or_ind_ptr) {
				*str_len_or_ind_ptr = sizeof(SQL_INTERVAL_STRUCT);
			}

			return SQL_SUCCESS;
		}
		case SQL_C_INTERVAL_DAY_TO_MINUTE: {
			interval_t interval;
			if (!OdbcInterval::GetInterval(val, interval, stmt)) {
				LogInvalidCast(val.type(), LogicalType::INTERVAL, stmt);
				return SQL_ERROR;
			}

			SQL_INTERVAL_STRUCT *interval_struct = (SQL_INTERVAL_STRUCT *)target_value_ptr;
			OdbcInterval::SetDayToMinute(interval, interval_struct);
			OdbcInterval::SetSignal(interval, interval_struct);

			if (str_len_or_ind_ptr) {
				*str_len_or_ind_ptr = sizeof(SQL_INTERVAL_STRUCT);
			}

			return SQL_SUCCESS;
		}
		case SQL_C_INTERVAL_DAY_TO_SECOND: {
			interval_t interval;
			if (!OdbcInterval::GetInterval(val, interval, stmt)) {
				LogInvalidCast(val.type(), LogicalType::INTERVAL, stmt);
				return SQL_ERROR;
			}

			SQL_INTERVAL_STRUCT *interval_struct = (SQL_INTERVAL_STRUCT *)target_value_ptr;
			OdbcInterval::SetDayToSecond(interval, interval_struct);
			OdbcInterval::SetSignal(interval, interval_struct);

			if (str_len_or_ind_ptr) {
				*str_len_or_ind_ptr = sizeof(SQL_INTERVAL_STRUCT);
			}

			return SQL_SUCCESS;
		}
		case SQL_C_INTERVAL_HOUR_TO_MINUTE: {
			interval_t interval;
			if (!OdbcInterval::GetInterval(val, interval, stmt)) {
				LogInvalidCast(val.type(), LogicalType::INTERVAL, stmt);
				return SQL_ERROR;
			}

			SQL_INTERVAL_STRUCT *interval_struct = (SQL_INTERVAL_STRUCT *)target_value_ptr;
			OdbcInterval::SetHourToMinute(interval, interval_struct);
			OdbcInterval::SetSignal(interval, interval_struct);

			if (str_len_or_ind_ptr) {
				*str_len_or_ind_ptr = sizeof(SQL_INTERVAL_STRUCT);
			}

			return SQL_SUCCESS;
		}
		case SQL_C_INTERVAL_HOUR_TO_SECOND: {
			interval_t interval;
			if (!OdbcInterval::GetInterval(val, interval, stmt)) {
				LogInvalidCast(val.type(), LogicalType::INTERVAL, stmt);
				return SQL_ERROR;
			}

			SQL_INTERVAL_STRUCT *interval_struct = (SQL_INTERVAL_STRUCT *)target_value_ptr;
			OdbcInterval::SetHourToSecond(interval, interval_struct);
			OdbcInterval::SetSignal(interval, interval_struct);

			if (str_len_or_ind_ptr) {
				*str_len_or_ind_ptr = sizeof(SQL_INTERVAL_STRUCT);
			}

			return SQL_SUCCESS;
		}
		case SQL_C_INTERVAL_MINUTE_TO_SECOND: {
			interval_t interval;
			if (!OdbcInterval::GetInterval(val, interval, stmt)) {
				LogInvalidCast(val.type(), LogicalType::INTERVAL, stmt);
				return SQL_ERROR;
			}

			SQL_INTERVAL_STRUCT *interval_struct = (SQL_INTERVAL_STRUCT *)target_value_ptr;
			OdbcInterval::SetMinuteToSecond(interval, interval_struct);
			OdbcInterval::SetSignal(interval, interval_struct);

			if (str_len_or_ind_ptr) {
				*str_len_or_ind_ptr = sizeof(SQL_INTERVAL_STRUCT);
			}

			return SQL_SUCCESS;
		}
		// TODO other types
		default:
			stmt->error_messages.emplace_back("Unsupported type.");
			return SQL_ERROR;

		} // end switch "(target_type)": SQL_C_TYPE_TIMESTAMP
	});
}
