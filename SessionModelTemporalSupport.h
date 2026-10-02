#pragma once

#include <string>

namespace SessionModelTemporalSupport
{
	std::string Trim(const std::string& value);

	bool IsValidTransactionTime(const std::string& value);

	bool IsValidTransactionWindow(const std::string& transactionStart, const std::string& transactionEnd);

	bool TransactionWindowVisibleAtSnapshot(const std::string& transactionStart,
	                                        const std::string& transactionEnd,
	                                        const std::string& snapshotTime);
}
