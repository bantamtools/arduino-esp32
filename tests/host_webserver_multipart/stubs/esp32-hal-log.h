#pragma once
// Host stub: log_e/log_w are counted, the rest are dropped.
extern int fake_log_errors;
#define log_e(...) (++fake_log_errors)
#define log_w(...) ((void)0)
#define log_i(...) ((void)0)
#define log_d(...) ((void)0)
#define log_v(...) ((void)0)
