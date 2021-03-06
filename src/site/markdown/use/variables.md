# PL/Java configuration variable reference

These PostgreSQL configuration variables can influence PL/Java's operation:

`dynamic_library_path`
: Although strictly not a PL/Java variable, `dynamic_library_path` influences
    where the PL/Java native code object (`.so`, `.dll`, `.bundle`, etc.) can
    be found, if the full path is not given to the `LOAD` command.

`server_encoding`
: Another non-PL/Java variable, this affects all text/character strings
    exchanged between PostgreSQL and Java. `UTF8` as the database and server
    encoding is _strongly_ recommended. If a different encoding is used, it
    should be any of the available _fully defined_ character encodings. In
    particular, the PostgreSQL pseudo-encoding `SQL_ASCII` (which means
    "characters within ASCII are ASCII, others are no-one-knows-what") will
    _not_ work well with PL/Java, raising exceptions whenever strings contain
    non-ASCII characters. (PL/Java can still be used in such a database, but
    the application code needs to know what it's doing and use the right
    conversion functions where needed.)

`pljava.classpath`
: The class path to be passed to the Java application class loader. There
    must be at least one (and usually only one) entry, the PL/Java jar file
    itself. To determine the proper setting, see
    [finding the files produced by a PL/Java build](../install/locate.html).

`pljava.debug`
: A boolean variable that, if set `on`, stops the process on first entry to
    PL/Java before the Java virtual machine is started. The process cannot
    proceed until a debugger is attached and used to set the static
    `Backend.c` variable `pljavaDebug` to zero. This may be useful for debugging
    PL/Java problems only seen in the context of some larger application
    that can't be stepped through.

`pljava.enable`
: Setting this variable `off` prevents PL/Java startup from completing, until
    the variable is later set `on`. It can be useful when
    [installing PL/Java on PostgreSQL versions before 9.2][pre92].

`pljava.implementors`
: A list of "implementor names" that PL/Java will recognize when processing
    [deployment descriptors][depdesc] inside a jar file being installed or
    removed. Deployment descriptors can contain commands with no implementor
    name, which will be executed always, or with an implementor name, executed
    only on a system recognizing that name. By default, this list contains only
    the entry `postgresql`. A deployment descriptor that contains commands with
    other implementor names can achieve a rudimentary kind of conditional
    execution if earlier commands adjust this list of names. _Commas separate
    elements of this list. Elements that are not regular identifiers need to be
    surrounded by double-quotes; prior to PostgreSQL 11, that syntax can be used
    directly in a `SET` command, while in 11 and after, such a value needs to be
    a (single-quoted) string explicitly containing the double quotes._

`pljava.java_thread_pg_entry`
: A choice of `allow`, `error`, `block`, or `throw` controlling PL/Java's thread
    management. Java makes heavy use of threading, while PostgreSQL may not be
    accessed by multiple threads concurrently. PL/Java's historical behavior is
    `allow`, which serializes access by Java threads into PostgreSQL, allowing
    a different Java thread in only when the current one calls or returns into
    Java. PL/Java formerly made some use of Java object finalizers, which
    required this approach, as finalizers run in their own thread.

    PL/Java itself no longer requires the ability for any thread to access
    PostgreSQL other than the original main thread. User code developed for
    PL/Java, however, may still rely on that ability. To test whether it does,
    the `error` or `throw` setting can be used here, and any attempt by a Java
    thread other
    than the main one to enter PostgreSQL will incur an exception (and stack
    trace, written to the server's standard error channel). When confident that
    there is no code that will need to enter PostgreSQL except on the main
    thread, the `block` setting can be used. That will eliminate PL/Java's
    frequent lock acquisitions and releases when the main thread crosses between
    PostgreSQL and Java, and will simply indefinitely block any other Java
    thread that attempts to enter PostgreSQL. This is an efficient setting, but
    can lead to blocked threads or a deadlocked backend if used with code that
    does attempt to access PG from more than one thread. (A JMX client, like
    JConsole, can identify the blocked threads, should that occur.)

    The `throw` setting is like `error` but more efficient: under the `error`
    setting, attempted entry by the wrong thread is detected in the native C
    code, only after a lock operation and call through JNI. Under the `throw`
    setting, the lock operations are elided and an entry attempt by the wrong
    thread results in no JNI call and an exception thrown directly in Java.

`pljava.libjvm_location`
: Used by PL/Java to load the Java runtime. The full path to a `libjvm` shared
    object (filename typically ending with `.so`, `.dll`, or `.dylib`).
    To determine the proper setting, see [finding the `libjvm` library][fljvm].

`pljava.release_lingering_savepoints`
: How the return from a PL/Java function will treat any savepoints created
    within it that have not been explicitly either released (the savepoint
    analog of "committed") or rolled back and released.
    If `off` (the default), they will be rolled back. If `on`, they will be
    released/committed. If possible, rather than setting this variable `on`,
    it would be safer to fix the function to release its own savepoints when
    appropriate.

    A savepoint continues to exist after being used as a rollback target.
    This is JDBC-specified behavior, but was not PL/Java's behavior before
    release 1.5.3, so code may exist that did not explicitly release or roll
    back a savepoint after rolling back to it once. To avoid a behavior change
    for such code, PL/Java will always release a savepoint that is still live
    at function return, regardless of this setting, if the savepoint has already
    been rolled back.

`pljava.statement_cache_size`
: The number of most-recently-prepared statements PL/Java will keep open.

`pljava.vmoptions`
: Any options to be passed to the Java runtime, in the same form as the
    documented options for the `java` command ([windows][jow],
    [Unix family][jou]). The string is split on whitespace unless found
    between single or double quotes. A backslash treats the following
    character literally, but the backslash itself remains in the string,
    so not all values can be expressed with these rules. If the server
    encoding is not `UTF8`, only ASCII characters should be used in
    `pljava.vmoptions`. The exact quoting and encoding rules for this variable
    may be adjusted in a future PL/Java version.

    Some important settings can be made here, and are described on the
    [VM options page][vmop].

[pre92]: ../install/prepg92.html
[depdesc]: https://github.com/tada/pljava/wiki/Sql-deployment-descriptor
[fljvm]: ../install/locatejvm.html
[jmx]: http://www.oracle.com/technetwork/articles/java/javamanagement-140525.html
[jvvm]: http://docs.oracle.com/javase/8/docs/technotes/guides/visualvm/
[jow]: https://docs.oracle.com/javase/8/docs/technotes/tools/windows/java.html
[jou]: https://docs.oracle.com/javase/8/docs/technotes/tools/unix/java.html
[vmop]: ../install/vmoptions.html
