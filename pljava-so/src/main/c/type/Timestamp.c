/*
 * Copyright (c) 2004-2018 Tada AB and other contributors, as listed below.
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the The BSD 3-Clause License
 * which accompanies this distribution, and is available at
 * http://opensource.org/licenses/BSD-3-Clause
 *
 * Contributors:
 *   Tada AB
 *   Thomas Hallgren
 *   PostgreSQL Global Development Group
 *   Chapman Flack
 */
#include <postgres.h>
#include <utils/nabstime.h>
#include <utils/datetime.h>

#include "pljava/Backend.h"
#include "pljava/type/Type_priv.h"
#include "pljava/type/Timestamp.h"

#define EPOCH_DIFF (((uint32)86400) * (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE))

/*
 * Types timestamp and timestamptz. This compilation unit supplies code for both
 * PostgreSQL types. The legacy JDBC mapping for both is to java.sql.Timestamp,
 * which holds an implicit timezone offset and therefore can't be an equally
 * good fit for both.
 *
 * Java 8 and JDBC 4.2 introduce java.time.LocalDateTime and
 * java.time.OffsetDateTime, which more directly fit PG's timestamp and
 * timestamptz, respectively. For compatibility reasons, the legacy behavior of
 * getObject (with no Class parameter) is unchanged, and still returns the data
 * weirdly shoehorned into java.sql.Timestamp. But Java 8 application code can
 * and should use the form of getObject with a Class parameter to request
 * java.time.LocalDateTime or java.time.OffsetDateTime, as appropriate.
 *
 * Note that it is somewhat misleading for PostgreSQL to call one of these types
 * TIMESTAMP WITH TIME ZONE. The stored form does not, in fact, include a time
 * zone (and this is in contrast to TIME WITH TIME ZONE, which does). Instead,
 * what PostgreSQL means by TIMESTAMP WITH TIMEZONE is that a zone can be given
 * (or inferred from the session) when a value is input, and used to adjust the
 * value to UTC, and, likewise, the stored UTC value can be output converted to
 * a given (or implicit) zone offset. Meanwhile, a TIMESTAMP WITHOUT TIME ZONE
 * is just stored as the number of seconds from epoch that would make a clock
 * on UTC show the same date and time as the value input.
 *
 * When producing a java.time.LocalDateTime from a timestamp and vice versa,
 * the conversion is just what you would think. When producing an OffsetDateTime
 * from a timestamptz, the OffsetDateTime will always have offset zero from UTC.
 * That's what the stored PostgreSQL data represents; to produce anything else
 * would be lying. When receiving an OffsetDateTime into PostgreSQL, of course
 * any zone offset it contains will be used to adjust the value to UTC for
 * storage.
 *
 * The legacy behavior when mapping timestamp and timestamptz to
 * java.sql.Timestamp is that a timestamptz is converted in both directions
 * without alteration, and a (local!) timestamp is *adjusted as if to UTC from
 * the current session's implicit timezone* (and vice versa when receiving
 * a value). Weird or not, that's how PL/Java has always done it.
 */
static jclass    s_Timestamp_class;
static jmethodID s_Timestamp_init;
static jmethodID s_Timestamp_getNanos;
static jmethodID s_Timestamp_getTime;
static jmethodID s_Timestamp_setNanos;

static TypeClass s_TimestampClass;
static TypeClass s_TimestamptzClass;

static TypeClass s_LocalDateTimeClass;
static TypeClass s_OffsetDateTimeClass;
/*
 * The following statics are specific to Java 8 +, and will be initialized
 * only on demand (pre-8 application code will have no way to demand them).
 */
static Type      s_LocalDateTimeInstance;
static jclass    s_LocalDateTime_class;
static jmethodID s_LocalDateTime_ofEpochSecond;
static jmethodID s_LocalDateTime_atOffset;
static Type      s_OffsetDateTimeInstance;
static jclass    s_OffsetDateTime_class;
static jmethodID s_OffsetDateTime_of;
static jmethodID s_OffsetDateTime_toEpochSecond;
static jmethodID s_OffsetDateTime_getNano;
static jobject   s_ZoneOffset_UTC;

/*
 * This only answers true for (same class or) TIMESTAMPOID.
 * The obtainer (below) only needs to construct and remember one instance.
 */
static bool _LocalDateTime_canReplaceType(Type self, Type other)
{
	TypeClass cls = Type_getClass(other);
	return Type_getClass(self) == cls  ||  Type_getOid(other) == TIMESTAMPOID;
}

static jvalue _LocalDateTime_coerceDatum(Type self, Datum arg)
{
	jlong micros;
	jint  onlyMicros;
	jlong secs;
	jvalue result;
#if PG_VERSION_NUM < 100000
	if ( !integerDateTimes )
	{
		/* 1e6 = 64 * 15625. *64 is a radix point shift, no precision loss */
		double shiftedFracSecs = DatumGetFloat8(arg) * 64;
		int64 shiftedSecs = (int64)floor(shiftedFracSecs);
		shiftedFracSecs -= (double)shiftedSecs;
		micros = (jlong)(15625*shiftedSecs);
		micros += ((jlong)floor(31250. * shiftedFracSecs) + 1) / 2;
	}
	else
#endif
	{
		micros = DatumGetInt64(arg);
	}
	/* Expect number of microseconds since 01 Jan 2000. Tease out a non-negative
	 * sub-second microseconds value (whether this C compiler's signed %
	 * has trunc or floor behavior).
	 */
	onlyMicros = (jint)(((micros % 1000000) + 1000000) % 1000000);
	secs = EPOCH_DIFF + (micros - onlyMicros) / 1000000;
	result.l = JNI_callStaticObjectMethod(s_LocalDateTime_class,
		s_LocalDateTime_ofEpochSecond,
		secs, 1000 * onlyMicros, s_ZoneOffset_UTC);
	return result;
}

static Datum _LocalDateTime_coerceObject(Type self, jobject timestamp)
{
	jobject offsetDateTime = JNI_callObjectMethod(timestamp,
		s_LocalDateTime_atOffset, s_ZoneOffset_UTC);
	jlong epochSec = JNI_callLongMethod(
		offsetDateTime, s_OffsetDateTime_toEpochSecond) - EPOCH_DIFF;
	jint nanos = JNI_callIntMethod(
		offsetDateTime, s_OffsetDateTime_getNano);
	Datum result;

#if PG_VERSION_NUM < 100000
	if ( !integerDateTimes )
	{
		double secs = (double)epochSec + ((double)nanos)/1e9;
		result = Float8GetDatum(secs);
	}
	else
#endif
	{
		result = Int64GetDatum(1000000L * epochSec + nanos / 1000);
	}

	JNI_deleteLocalRef(offsetDateTime);
	return result;
}

static Type _LocalDateTime_obtain(Oid typeId)
{
	if ( NULL == s_LocalDateTimeInstance )
	{
		jclass zoneOffsetCls = PgObject_getJavaClass("java/time/ZoneOffset");
		jfieldID fldUTC = PgObject_getStaticJavaField(
			zoneOffsetCls, "UTC", "Ljava/time/ZoneOffset;");
		s_ZoneOffset_UTC = JNI_newGlobalRef(JNI_getStaticObjectField(
			zoneOffsetCls, fldUTC));
		JNI_deleteLocalRef(zoneOffsetCls);

		s_LocalDateTime_class = JNI_newGlobalRef(PgObject_getJavaClass(
			"java/time/LocalDateTime"));
		s_LocalDateTime_ofEpochSecond = PgObject_getStaticJavaMethod(
			s_LocalDateTime_class, "ofEpochSecond",
			"(JILjava/time/ZoneOffset;)Ljava/time/LocalDateTime;");
		s_LocalDateTime_atOffset = PgObject_getJavaMethod(s_LocalDateTime_class,
			"atOffset", "(Ljava/time/ZoneOffset;)Ljava/time/OffsetDateTime;");

		s_OffsetDateTime_class = JNI_newGlobalRef(PgObject_getJavaClass(
			"java/time/OffsetDateTime"));
		s_OffsetDateTime_toEpochSecond = PgObject_getJavaMethod(
			s_OffsetDateTime_class, "toEpochSecond", "()J");
		s_OffsetDateTime_getNano = PgObject_getJavaMethod(
			s_OffsetDateTime_class, "getNano", "()I");

		/*
		 * Haven't initialized s_OffsetDateTime_of, but _OffsetDateTime_obtain
		 * will.
		 */

		s_LocalDateTimeInstance =
			TypeClass_allocInstance(s_LocalDateTimeClass, TIMESTAMPOID);
	}
	return s_LocalDateTimeInstance;
}

static bool _Timestamp_canReplaceType(Type self, Type other)
{
	TypeClass cls = Type_getClass(other);
	return Type_getClass(self) == cls || cls == s_TimestamptzClass;
}

static jvalue Timestamp_coerceDatumTZ_id(Type self, Datum arg, bool tzAdjust)
{
	jvalue result;
	int64 ts = DatumGetInt64(arg);

	/* Expect number of microseconds since 01 Jan 2000. Tease out a non-negative
	 * sub-second microseconds value (whether this C compiler's signed %
	 * has trunc or floor behavior).
	 */
	jint  uSecs = (jint)(((ts % 1000000) + 1000000) % 1000000);
	jlong mSecs = (ts - uSecs) / 1000; /* Convert to millisecs */

	if(tzAdjust)
	{
		int tz = Timestamp_getTimeZone_id(ts);
		mSecs += tz * 1000; /* Adjust from local time to UTC */
	}

	/* Adjust for diff between Postgres and Java (Unix) */
	mSecs += ((jlong)EPOCH_DIFF) * 1000L;

	result.l = JNI_newObject(s_Timestamp_class, s_Timestamp_init, mSecs);
	if(uSecs != 0)
		JNI_callVoidMethod(result.l, s_Timestamp_setNanos, uSecs * 1000);
	return result;
}

#if PG_VERSION_NUM < 100000
static jvalue Timestamp_coerceDatumTZ_dd(Type self, Datum arg, bool tzAdjust)
{
	jlong mSecs;
	jint  uSecs;
	jvalue result;
	double ts = DatumGetFloat8(arg);
	int    tz = Timestamp_getTimeZone_dd(ts);

	/* Expect <seconds since Jan 01 2000>.<fractions of seconds>
	 */
	if(tzAdjust)
		ts += tz; /* Adjust from local time to UTC */
	ts += EPOCH_DIFF; /* Adjust for diff between Postgres and Java (Unix) */
	mSecs = (jlong) floor(ts * 1000.0); /* Convert to millisecs */
	uSecs = (jint) ((ts - floor(ts)) * 1000000.0); /* Preserve microsecs */
	result.l = JNI_newObject(s_Timestamp_class, s_Timestamp_init, mSecs);
	if(uSecs != 0)
		JNI_callVoidMethod(result.l, s_Timestamp_setNanos, uSecs * 1000);
	return result;
}
#endif

static jvalue Timestamp_coerceDatumTZ(Type self, Datum arg, bool tzAdjust)
{
	return
#if PG_VERSION_NUM < 100000
		(!integerDateTimes) ? Timestamp_coerceDatumTZ_dd(self, arg, tzAdjust) :
#endif
		Timestamp_coerceDatumTZ_id(self, arg, tzAdjust);
}

static Datum Timestamp_coerceObjectTZ_id(Type self, jobject jts, bool tzAdjust)
{
	int64 ts;
	jlong mSecs = JNI_callLongMethod(jts, s_Timestamp_getTime);
	jint  nSecs = JNI_callIntMethod(jts, s_Timestamp_getNanos);
	/*
	 * getNanos() should have supplied non-negative nSecs, whether mSecs is
	 * positive or negative. So mSecs needs to be floor()ed to a multiple of
	 * 1000 ms, whether this C compiler does signed integer division with floor
	 * or trunc.
	 */
	mSecs -= ((mSecs % 1000) + 1000) % 1000;
	mSecs -= ((jlong)EPOCH_DIFF) * 1000L;
	ts  = mSecs * 1000L; /* Convert millisecs to microsecs */
	if(nSecs != 0)
		ts += nSecs / 1000;	/* Convert nanosecs  to microsecs */
	if(tzAdjust)
		ts -= ((jlong)Timestamp_getTimeZone_id(ts)) * 1000000L; /* Adjust from UTC to local time */
	return Int64GetDatum(ts);
}

#if PG_VERSION_NUM < 100000
static Datum Timestamp_coerceObjectTZ_dd(Type self, jobject jts, bool tzAdjust)
{
	double ts;
	jlong mSecs = JNI_callLongMethod(jts, s_Timestamp_getTime);
	jint  nSecs = JNI_callIntMethod(jts, s_Timestamp_getNanos);
	ts = ((double)mSecs) / 1000.0; /* Convert to seconds */
	ts -= EPOCH_DIFF;
	if(nSecs != 0)
		ts += ((double)nSecs) / 1000000000.0;	/* Convert to seconds */
	if(tzAdjust)
		ts -= Timestamp_getTimeZone_dd(ts); /* Adjust from UTC to local time */
	return Float8GetDatum(ts);
}
#endif

static Datum Timestamp_coerceObjectTZ(Type self, jobject jts, bool tzAdjust)
{
	return
#if PG_VERSION_NUM < 100000
		(!integerDateTimes) ? Timestamp_coerceObjectTZ_dd(self, jts, tzAdjust) :
#endif
		Timestamp_coerceObjectTZ_id(self, jts, tzAdjust);
}

static jvalue _Timestamp_coerceDatum(Type self, Datum arg)
{
	return Timestamp_coerceDatumTZ(self, arg, true);
}

static Datum _Timestamp_coerceObject(Type self, jobject ts)
{
	return Timestamp_coerceObjectTZ(self, ts, true);
}

/*
 * Timestamp with time zone. Basically same as Timestamp but postgres will pass
 * this one in GMT timezone so there's no without ajustment for time zone.
 */
static bool _Timestamptz_canReplaceType(Type self, Type other)
{
	TypeClass cls = Type_getClass(other);
	return Type_getClass(self) == cls || cls == s_TimestampClass;
}

static jvalue _Timestamptz_coerceDatum(Type self, Datum arg)
{
	return Timestamp_coerceDatumTZ(self, arg, false);
}

static Datum _Timestamptz_coerceObject(Type self, jobject ts)
{
	return Timestamp_coerceObjectTZ(self, ts, false);
}

static int32 Timestamp_getTimeZone(pg_time_t time)
{
#if defined(_MSC_VER) && ( \
	100000<=PG_VERSION_NUM && PG_VERSION_NUM<102000 || \
	 90600<=PG_VERSION_NUM && PG_VERSION_NUM< 90607 || \
	 90500<=PG_VERSION_NUM && PG_VERSION_NUM< 90511 || \
	 90400<=PG_VERSION_NUM && PG_VERSION_NUM< 90416 || \
	PG_VERSION_NUM < 90321 )
	/* This is gross, but pg_tzset has a cache, so not as gross as you think.
	 * There is some renewed interest on pgsql-hackers to find a good answer for
	 * the MSVC PGDLLIMPORT nonsense, so this may not have to stay gross.
	 */
	char const *tzname = PG_GETCONFIGOPTION("timezone");
	struct pg_tm* tx = pg_localtime(&time, pg_tzset(tzname));
#elif PG_VERSION_NUM < 80300
	struct pg_tm* tx = pg_localtime(&time, global_timezone);
#else
	struct pg_tm* tx = pg_localtime(&time, session_timezone);
#endif
	return -(int32)tx->tm_gmtoff;
}

int32 Timestamp_getTimeZone_id(int64 dt)
{
	return Timestamp_getTimeZone(
		(dt / INT64CONST(1000000) + (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * 86400));
}
#if PG_VERSION_NUM < 100000
int32 Timestamp_getTimeZone_dd(double dt)
{
	return Timestamp_getTimeZone(
		(pg_time_t)rint(dt + (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * 86400));
}
#endif

int32 Timestamp_getCurrentTimeZone(void)
{
	return Timestamp_getTimeZone((pg_time_t)GetCurrentAbsoluteTime());
}

extern void Timestamp_initialize(void);
void Timestamp_initialize(void)
{
	TypeClass cls;
	s_Timestamp_class = JNI_newGlobalRef(PgObject_getJavaClass("java/sql/Timestamp"));
	s_Timestamp_init = PgObject_getJavaMethod(s_Timestamp_class, "<init>", "(J)V");
	s_Timestamp_getNanos = PgObject_getJavaMethod(s_Timestamp_class, "getNanos", "()I");
	s_Timestamp_getTime  = PgObject_getJavaMethod(s_Timestamp_class, "getTime",  "()J");
	s_Timestamp_setNanos = PgObject_getJavaMethod(s_Timestamp_class, "setNanos", "(I)V");

	cls = TypeClass_alloc("type.Timestamp");
	cls->JNISignature   = "Ljava/sql/Timestamp;";
	cls->javaTypeName   = "java.sql.Timestamp";
	cls->canReplaceType = _Timestamp_canReplaceType;
	cls->coerceDatum    = _Timestamp_coerceDatum;
	cls->coerceObject   = _Timestamp_coerceObject;
	Type_registerType(0, TypeClass_allocInstance(cls, TIMESTAMPOID));
	s_TimestampClass = cls;

	cls = TypeClass_alloc("type.Timestamptz");
	cls->JNISignature   = "Ljava/sql/Timestamp;";
	cls->javaTypeName   = "java.sql.Timestamp";
	cls->canReplaceType = _Timestamptz_canReplaceType;
	cls->coerceDatum    = _Timestamptz_coerceDatum;
	cls->coerceObject   = _Timestamptz_coerceObject;
	Type_registerType("java.sql.Timestamp", TypeClass_allocInstance(cls, TIMESTAMPTZOID));
	s_TimestamptzClass = cls;

	cls = TypeClass_alloc("type.LocalDateTime");
	cls->JNISignature = "Ljava/time/LocalDateTime;";
	cls->javaTypeName = "java.time.LocalDateTime";
	cls->coerceDatum  = _LocalDateTime_coerceDatum;
	cls->coerceObject = _LocalDateTime_coerceObject;
	cls->canReplaceType = _LocalDateTime_canReplaceType;
	s_LocalDateTimeClass  = cls;
	Type_registerType2(InvalidOid, "java.time.LocalDateTime",
		_LocalDateTime_obtain);
}
