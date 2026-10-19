#pragma once
#define AV_LOG_ERROR 16
#define AV_LOG_WARNING 24
#define AV_LOG_INFO 32
#define AV_LOG_DEBUG 48
#define av_log(ctx, lvl, ...) ((void)(ctx), (void)(lvl))
#define avpriv_report_missing_feature(ctx, ...) ((void)(ctx))
