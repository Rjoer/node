#pragma once

//#define MONITOR_NODE
//#define SPAMMER
//#define SPAM_MAIN
//#define STARTER

//#define AJAX_IFACE
#define CUSTOMER_NODE
//#define FOREVER_ALONE
#define TIME_TO_COLLECT_TRXNS 200
#define TIME_TO_AWAIT_ACTIVITY 300
#define TRX_SLEEP_TIME 50000 //microseconds
#define FAKE_BLOCKS

#if ! defined(DEBUG)
#define SPAMMER
#endif

#define SYNCRO
#define MYLOG

#define BOTTLENECKED_SMARTS
#define AJAX_CONCURRENT_API_CLIENTS INT64_MAX
#define BINARY_TCP_API
#define DEFAULT_CURRENCY 1

constexpr auto SIZE_OF_COMMON_TRANSACTION = 190;
constexpr auto COST_OF_ONE_TRUSTED_PER_DAY = 17.;