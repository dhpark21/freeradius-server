/*
 *	We have an error, so we have common error handling code.
 */
switch (errno) {
case EINTR:
	/*
	 *	Try a few times before giving up.
	 */
	tries++;
	if (tries <= my->max_tries) goto retry;
	return 0;

#if defined(EWOULDBLOCK) && (EWOULDBLOCK != EAGAIN)
case EWOULDBLOCK:
#endif
case EAGAIN:
	/*
	 *	The operation would block, return that.
	 */
	if (!my->info.flag_blocked) {
		if (my->cb.flag_blocked) {
			rcode = my->cb.flag_blocked((fr_bio_t *) my);
			if (rcode < 0) return rcode;

			my->info.flag_blocked = true;
		}
	}
	return fr_bio_error(IO_WOULD_BLOCK);

#ifndef NDEBUG
case ENOTCONN:
	/*
	 *	We're doing a read/write to a socket which isn't connected.  This is a failure of the
	 *	application state machine.
	 */
	fr_assert(0);
	break;
#endif

case ECONNRESET:
case EPIPE:
	/*
	 *	The other end closed the connection, signal the application that it's a (maybe) clean close,
	 *	and set EOF on the BIO.
	 */
	fr_bio_eof(&my->bio);
	if (my->cb.eof) my->cb.eof(&my->bio); /* inform the application that we're at EOF */
	return 0;

default:
	/*
	 *	Some other error, it's fatal.
	 */
	break;
}
