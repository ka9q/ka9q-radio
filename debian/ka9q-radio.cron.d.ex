#
# Regular cron jobs for the ka9q-radio package.
#
0 4	* * *	root	[ -x /usr/bin/ka9q-radio_maintenance ] && /usr/bin/ka9q-radio_maintenance
