#include "wil6210.h"
#include "trace.h"

int wil_err(struct wil6210_priv *wil, const char *fmt, ...)
{
	struct net_device *ndev = wil_to_ndev(wil);
	struct va_format vaf = {
		.fmt = fmt,
	};
	va_list args;
	int ret;

	va_start(args, fmt);
	vaf.va = &args;
	ret = netdev_err(ndev, "%pV", &vaf);
	trace_wil6210_log_err(&vaf);
	va_end(args);

	return ret;
}

int wil_info(struct wil6210_priv *wil, const char *fmt, ...)
{
	struct net_device *ndev = wil_to_ndev(wil);
	struct va_format vaf = {
		.fmt = fmt,
	};
	va_list args;
	int ret;

	va_start(args, fmt);
	vaf.va = &args;
	ret = netdev_info(ndev, "%pV", &vaf);
	trace_wil6210_log_info(&vaf);
	va_end(args);

	return ret;
}

int wil_dbg_trace(struct wil6210_priv *wil, const char *fmt, ...)
{
	struct va_format vaf = {
		.fmt = fmt,
	};
	va_list args;

	va_start(args, fmt);
	vaf.va = &args;
	trace_wil6210_log_dbg(&vaf);
	va_end(args);

	return 0;
}

