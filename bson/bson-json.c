/*
 * Copyright 2013 MongoDB Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "bson.h"
#include "bson-json.h"
#include "b64_pton.h"
#include "yajl/yajl_parse.h"
#include "yajl/yajl_parser.h"
#include "yajl/yajl_bytestack.h"

#define STACK_MAX 100
#define BSON_JSON_DEFAULT_BUF_SIZE 1 << 14

typedef enum
{
   BSON_JSON_REGULAR,
   BSON_JSON_DONE,
   BSON_JSON_ERROR,
   BSON_JSON_IN_START_MAP,
   BSON_JSON_IN_BSON_TYPE,
   BSON_JSON_IN_BSON_TYPE_TIMESTAMP_STARTMAP,
   BSON_JSON_IN_BSON_TYPE_TIMESTAMP_VALUES,
   BSON_JSON_IN_BSON_TYPE_TIMESTAMP_ENDMAP,
} _bson_json_read_state_t;

typedef enum
{
   BSON_JSON_LF_REGEX,
   BSON_JSON_LF_OPTIONS,
   BSON_JSON_LF_OID,
   BSON_JSON_LF_BINARY,
   BSON_JSON_LF_TYPE,
   BSON_JSON_LF_DATE,
   BSON_JSON_LF_TIMESTAMP_T,
   BSON_JSON_LF_TIMESTAMP_I,
   BSON_JSON_LF_REF,
   BSON_JSON_LF_ID,
   BSON_JSON_LF_UNDEFINED,
   BSON_JSON_LF_MINKEY,
   BSON_JSON_LF_MAXKEY,
} _bson_json_read_bson_state_t;

typedef struct
{
   uint8_t *buf;
   size_t   n_bytes;
   size_t   len;
} _bson_json_buf_t;

typedef struct
{
   int    i;
   bool   is_array;
   bson_t bson;
} _bson_json_stack_frame_t;

typedef union {
   struct
   {
      bool has_regex;
      bool has_options;
   } regex;
   struct
   {
      bool       has_oid;
      bson_oid_t oid;
   } oid;
   struct
   {
      bool           has_binary;
      bool           has_subtype;
      bson_subtype_t type;
   } binary;
   struct
   {
      bool    has_date;
      int64_t date;
   } date;
   struct
   {
      bool     has_t;
      bool     has_i;
      uint32_t t;
      uint32_t i;
   } timestamp;
   struct
   {
      bool       has_ref;
      bool       has_id;
      bson_oid_t id;
   } ref;
   struct
   {
      bool has_undefined;
   } undefined;
   struct
   {
      bool has_minkey;
   } minkey;
   struct
   {
      bool has_maxkey;
   } maxkey;
} _bson_json_bson_data_t;

typedef struct
{
   bson_t                      *bson;
   _bson_json_stack_frame_t     stack[STACK_MAX];
   int                          n;
   const char                  *key;
   _bson_json_buf_t             key_buf;
   _bson_json_read_state_t      read_state;
   _bson_json_read_bson_state_t bson_state;
   bson_type_t                  bson_type;
   _bson_json_buf_t             bson_type_buf[3];
   _bson_json_bson_data_t       bson_type_data;
   bool                         known_bson_type;
} _bson_json_reader_bson_t;

typedef struct
{
   void                *data;
   bson_json_reader_cb  cb;
   bson_json_destroy_cb dcb;
   uint8_t             *buf;
   size_t               buf_size;
   size_t               bytes_read;
   size_t               bytes_parsed;
} _bson_json_reader_producer_t;

struct _bson_json_reader
{
   _bson_json_reader_producer_t producer;
   _bson_json_reader_bson_t     bson;
   yajl_handle                  yh;
   bson_error_t                *error;
};

#define STACK_ELE(_delta, _name) (bson->stack[(_delta) + bson->n]._name)

#define STACK_BSON(_delta) ( \
      ((_delta) + bson->n) == 0 \
      ? bson->bson \
      : &STACK_ELE (_delta, bson) \
      )

#define STACK_BSON_PARENT STACK_BSON (-1)
#define STACK_BSON_CHILD STACK_BSON (0)

#define STACK_I STACK_ELE (0, i)
#define STACK_IS_ARRAY STACK_ELE (0, is_array)

#define STACK_PUSH_ARRAY(statement) \
   do { \
      if (bson->n >= (STACK_MAX - 1)) { return 0; } \
      if (bson->n == -1) { return 0; } \
      bson->n++; \
      STACK_I = 0; \
      STACK_IS_ARRAY = 1; \
      statement; \
   } while (0)

#define STACK_PUSH_DOC(statement) \
   do { \
      if (bson->n >= (STACK_MAX - 1)) { return 0; } \
      bson->n++; \
      if (bson->n != 0) { \
         STACK_IS_ARRAY = 0; \
         statement; \
      } \
   } while (0)

#define STACK_POP_ARRAY(statement) \
   do { \
      if (!STACK_IS_ARRAY) { return 0; } \
      if (bson->n <= 0) { return 0; } \
      statement; \
      bson->n--; \
   } while (0)

#define STACK_POP_DOC(statement) \
   do { \
      if (STACK_IS_ARRAY) { return 0; } \
      if (bson->n < 0) { return 0; } \
      if (bson->n > 0) { \
         statement; \
      } \
      bson->n--; \
   } while (0)

static void
_bson_json_read_set_error (bson_json_reader_t *reader,
                           const char         *fmt,
                           ...)
{
   va_list ap;

   if (reader->error) {
      va_start (ap, fmt);

      reader->error->domain = BSON_JSON_ERROR_READ;
      reader->error->code = BSON_JSON_ERROR_READ_INVALID_PARAM;

      bson_vsnprintf (reader->error->message, sizeof reader->error->message,
                      fmt, ap);

      va_end (ap);

      reader->error->message[sizeof reader->error->message - 1] = '\0';
   }

   reader->bson.read_state = BSON_JSON_ERROR;
}

static void
_bson_json_buf_ensure (_bson_json_buf_t *buf,
                       size_t            len)
{
   if (buf->n_bytes < len) {
      free (buf->buf);

      buf->n_bytes = bson_next_power_of_two (len);
      buf->buf = malloc (buf->n_bytes);
   }
}

static void
_bson_json_read_fixup_key (_bson_json_reader_bson_t *bson)
{
   if (bson->n > 0 && STACK_IS_ARRAY) {
      _bson_json_buf_ensure (&bson->key_buf, 12);
      bson->key_buf.len = bson_uint32_to_string (STACK_I, &bson->key,
                                                 (char *)bson->key_buf.buf, 12);
      STACK_I++;
   }
}

static void
_bson_json_buf_set (_bson_json_buf_t *buf,
                    const void       *from,
                    size_t            len,
                    bool              trailing_null)
{
   if (trailing_null) {
      _bson_json_buf_ensure (buf, len + 1);
   } else {
      _bson_json_buf_ensure (buf, len);
   }

   memcpy (buf->buf, from, len);

   if (trailing_null) {
      buf->buf[len] = '\0';
   }

   buf->len = len;
}

#define BASIC_YAJL_CB_PREAMBLE \
   const char *key; \
   size_t len; \
   bson_json_reader_t *reader = (bson_json_reader_t *)_ctx; \
   _bson_json_reader_bson_t *bson = &reader->bson; \
   _bson_json_read_fixup_key (bson); \
   key = bson->key; \
   len = bson->key_buf.len;

#define BASIC_YAJL_CB_BAIL_IF_NOT_NORMAL(_type) \
   if (bson->read_state != BSON_JSON_REGULAR) { \
      _bson_json_read_set_error (reader, "Invalid read of %s in state %d", \
                                 (_type), bson->read_state); \
      return 0; \
   }

static int
_bson_json_read_null (void *_ctx)
{
   BASIC_YAJL_CB_PREAMBLE;
   BASIC_YAJL_CB_BAIL_IF_NOT_NORMAL ("null");

   bson_append_null (STACK_BSON_CHILD, key, len);

   return 1;
}

static int
_bson_json_read_boolean (void *_ctx,
                         int   val)
{
   BASIC_YAJL_CB_PREAMBLE;

   if (bson->read_state == BSON_JSON_IN_BSON_TYPE && bson->bson_state ==
       BSON_JSON_LF_UNDEFINED) {
      bson->bson_type_data.undefined.has_undefined = true;
      return 1;
   }

   BASIC_YAJL_CB_BAIL_IF_NOT_NORMAL ("boolean");

   bson_append_bool (STACK_BSON_CHILD, key, len, val);

   return 1;
}

static int
_bson_json_read_integer (void     *_ctx,
                         long long val)
{
   _bson_json_read_state_t rs;
   _bson_json_read_bson_state_t bs;

   BASIC_YAJL_CB_PREAMBLE;

   rs = bson->read_state;
   bs = bson->bson_state;

   if (rs == BSON_JSON_REGULAR) {
      if (abs (val) <= INT32_MAX) {
         bson_append_int32 (STACK_BSON_CHILD, key, len, val);
      } else {
         bson_append_int64 (STACK_BSON_CHILD, key, len, val);
      }
   } else if (rs == BSON_JSON_IN_BSON_TYPE || rs ==
              BSON_JSON_IN_BSON_TYPE_TIMESTAMP_VALUES) {
      switch (bs) {
      case BSON_JSON_LF_DATE:
         bson->bson_type_data.date.has_date = true;
         bson->bson_type_data.date.date = val;
         break;
      case BSON_JSON_LF_TIMESTAMP_T:
         bson->bson_type_data.timestamp.has_t = true;
         bson->bson_type_data.timestamp.t = val;
         break;
      case BSON_JSON_LF_TIMESTAMP_I:
         bson->bson_type_data.timestamp.has_i = true;
         bson->bson_type_data.timestamp.i = val;
         break;
      case BSON_JSON_LF_MINKEY:
         bson->bson_type_data.minkey.has_minkey = true;
         break;
      case BSON_JSON_LF_MAXKEY:
         bson->bson_type_data.maxkey.has_maxkey = true;
         break;
      case BSON_JSON_LF_REGEX:
      case BSON_JSON_LF_OPTIONS:
      case BSON_JSON_LF_OID:
      case BSON_JSON_LF_BINARY:
      case BSON_JSON_LF_TYPE:
      case BSON_JSON_LF_REF:
      case BSON_JSON_LF_ID:
      case BSON_JSON_LF_UNDEFINED:
      default:
         _bson_json_read_set_error (reader,
                                    "Invalid special type for integer read %d",
                                    bs);
         return 0;
      }
   } else {
      _bson_json_read_set_error (reader, "Invalid state for integer read %d",
                                 rs);
      return 0;
   }

   return 1;
}

static int
_bson_json_read_double (void  *_ctx,
                        double val)
{
   BASIC_YAJL_CB_PREAMBLE;
   BASIC_YAJL_CB_BAIL_IF_NOT_NORMAL ("double");

   bson_append_double (STACK_BSON_CHILD, key, len, val);

   return 1;
}

static int
_bson_json_read_string (void                *_ctx,
                        const unsigned char *val,
                        size_t               vlen)
{
   _bson_json_read_state_t rs;
   _bson_json_read_bson_state_t bs;

   BASIC_YAJL_CB_PREAMBLE;

   rs = bson->read_state;
   bs = bson->bson_state;

   if (rs == BSON_JSON_REGULAR) {
      bson_append_utf8 (STACK_BSON_CHILD, key, len, (const char *)val, vlen);
   } else if (rs == BSON_JSON_IN_BSON_TYPE || rs ==
              BSON_JSON_IN_BSON_TYPE_TIMESTAMP_VALUES) {
      const char *val_w_null;
      _bson_json_buf_set (&bson->bson_type_buf[2], val, vlen, true);
      val_w_null = (const char *)bson->bson_type_buf[2].buf;

      switch (bs) {
      case BSON_JSON_LF_REGEX:
         bson->bson_type_data.regex.has_regex = true;
         _bson_json_buf_set (&bson->bson_type_buf[0], val, vlen, true);
         break;
      case BSON_JSON_LF_OPTIONS:
         bson->bson_type_data.regex.has_options = true;
         _bson_json_buf_set (&bson->bson_type_buf[1], val, vlen, true);
         break;
      case BSON_JSON_LF_OID:

         if (vlen != 24) {
            goto BAD_PARSE;
         }

         bson->bson_type_data.oid.has_oid = true;
         bson_oid_init_from_string (&bson->bson_type_data.oid.oid, val_w_null);
         break;
      case BSON_JSON_LF_TYPE:
         bson->bson_type_data.binary.has_subtype = true;

         if (sscanf (val_w_null, "%02x",
                     &bson->bson_type_data.binary.type) != 1) {
            goto BAD_PARSE;
         }

         break;
      case BSON_JSON_LF_BINARY: {
            /* TODO: error handling for pton */
            int binary_len;
            bson->bson_type_data.binary.has_binary = true;
            binary_len = b64_pton (val_w_null, NULL, 0);
            _bson_json_buf_ensure (&bson->bson_type_buf[0], binary_len + 1);
            b64_pton ((char *)bson->bson_type_buf[2].buf,
                      bson->bson_type_buf[0].buf, binary_len + 1);
            bson->bson_type_buf[0].len = binary_len;
            break;
         }
      case BSON_JSON_LF_REF:
         bson->bson_type_data.ref.has_ref = true;
         _bson_json_buf_set (&bson->bson_type_buf[0], val, vlen, true);
         break;
      case BSON_JSON_LF_ID:

         if (vlen != 24) {
            goto BAD_PARSE;
         }

         bson->bson_type_data.ref.has_id = true;
         bson_oid_init_from_string (&bson->bson_type_data.ref.id, val_w_null);
         break;
      case BSON_JSON_LF_DATE:
      case BSON_JSON_LF_TIMESTAMP_T:
      case BSON_JSON_LF_TIMESTAMP_I:
      case BSON_JSON_LF_UNDEFINED:
      case BSON_JSON_LF_MINKEY:
      case BSON_JSON_LF_MAXKEY:
      default:
         goto BAD_PARSE;
      }

      return 1;

   BAD_PARSE:
      _bson_json_read_set_error (reader,
                                 "Invalid input string %s, looking for %d",
                                 val_w_null, bs);
      return 0;
   } else {
      _bson_json_read_set_error (reader, "Invalid state to look for string %d",
                                 rs);
      return 0;
   }

   return 1;
}

static int
_bson_json_read_start_map (void *_ctx)
{
   BASIC_YAJL_CB_PREAMBLE;

   if (bson->read_state == BSON_JSON_IN_BSON_TYPE_TIMESTAMP_STARTMAP) {
      bson->read_state = BSON_JSON_IN_BSON_TYPE_TIMESTAMP_VALUES;
   } else {
      bson->read_state = BSON_JSON_IN_START_MAP;
   }

   /* silence some warnings */
   (void)len;
   (void)key;

   return 1;
}

#define HANDLE_OPTION(_key, _type, _state) \
   (len == strlen (_key) && memcmp (val, (_key), len) == 0) { \
      if (bson->known_bson_type && bson->bson_type != (_type)) { \
         _bson_json_read_set_error (reader, \
                                    "Invalid key %s.  Looking for values for %d", \
                                    (_key), bson->bson_type); \
         return 0; \
      } \
      bson->bson_type = (_type); \
      bson->bson_state = (_state); \
   }

static int
_bson_json_read_map_key (void                *_ctx,
                         const unsigned char *val,
                         size_t               len)
{
   bson_json_reader_t *reader = (bson_json_reader_t *)_ctx;
   _bson_json_reader_bson_t *bson = &reader->bson;

   if (bson->read_state == BSON_JSON_IN_START_MAP) {
      if (len > 0 && val[0] == '$') {
         bson->read_state = BSON_JSON_IN_BSON_TYPE;
         bson->bson_type = 0;
         memset (&bson->bson_type_data, sizeof bson->bson_type_data, 0);
      } else {
         bson->read_state = BSON_JSON_REGULAR;
         STACK_PUSH_DOC (bson_append_document_begin (STACK_BSON_PARENT,
                                                     bson->key,
                                                     bson->key_buf.len,
                                                     STACK_BSON_CHILD));
      }
   }

   if (bson->read_state == BSON_JSON_IN_BSON_TYPE) {
      if HANDLE_OPTION ("$regex", BSON_TYPE_REGEX, BSON_JSON_LF_REGEX) else
      if HANDLE_OPTION ("$options", BSON_TYPE_REGEX, BSON_JSON_LF_OPTIONS) else
      if HANDLE_OPTION ("$oid", BSON_TYPE_OID, BSON_JSON_LF_OID) else
      if HANDLE_OPTION ("$binary", BSON_TYPE_BINARY, BSON_JSON_LF_BINARY) else
      if HANDLE_OPTION ("$type", BSON_TYPE_BINARY, BSON_JSON_LF_TYPE) else
      if HANDLE_OPTION ("$date", BSON_TYPE_DATE_TIME, BSON_JSON_LF_DATE) else
      if HANDLE_OPTION ("$ref", BSON_TYPE_DBPOINTER, BSON_JSON_LF_REF) else
      if HANDLE_OPTION ("$id", BSON_TYPE_DBPOINTER, BSON_JSON_LF_ID) else
      if HANDLE_OPTION ("$undefined", BSON_TYPE_UNDEFINED,
                        BSON_JSON_LF_UNDEFINED) else
      if HANDLE_OPTION ("$minkey", BSON_TYPE_MINKEY, BSON_JSON_LF_MINKEY) else
      if HANDLE_OPTION ("$maxkey", BSON_TYPE_MAXKEY, BSON_JSON_LF_MAXKEY) else
      if (len == strlen ("$timestamp") &&
          memcmp (val, "$timestamp", len) == 0) {
         bson->bson_type = BSON_TYPE_TIMESTAMP;
         bson->read_state = BSON_JSON_IN_BSON_TYPE_TIMESTAMP_STARTMAP;
      } else {
         _bson_json_read_set_error (reader,
                                    "Invalid key %s.  Looking for values for %d",
                                    val, bson->bson_type);
         return 0;
      }
   } else if (bson->read_state == BSON_JSON_IN_BSON_TYPE_TIMESTAMP_VALUES) {
      if HANDLE_OPTION ("t", BSON_TYPE_TIMESTAMP, BSON_JSON_LF_TIMESTAMP_T) else
      if HANDLE_OPTION ("i", BSON_TYPE_TIMESTAMP,
                        BSON_JSON_LF_TIMESTAMP_I) else {
         _bson_json_read_set_error (reader,
                                    "Invalid key %s.  Looking for values for %d",
                                    val, bson->bson_type);
         return 0;
      }
   } else {
      _bson_json_buf_set (&bson->key_buf, val, len, true);
      bson->key = (const char *)bson->key_buf.buf;
   }

   return 1;
}

static int
_bson_json_read_append_binary (bson_json_reader_t       *reader,
                               _bson_json_reader_bson_t *bson)
{
   if (!bson->bson_type_data.binary.has_binary) {
      _bson_json_read_set_error (reader,
                                 "Missing $binary after $type in BSON_TYPE_BINARY");
   } else if (!bson->bson_type_data.binary.has_subtype) {
      _bson_json_read_set_error (reader,
                                 "Missing $type after $binary in BSON_TYPE_BINARY");
   } else {
      return bson_append_binary (STACK_BSON_CHILD, bson->key, bson->key_buf.len,
                                 bson->bson_type_data.binary.type,
                                 bson->bson_type_buf[0].buf,
                                 bson->bson_type_buf[0].len);
   }

   return 0;
}

static int
_bson_json_read_append_regex (bson_json_reader_t       *reader,
                              _bson_json_reader_bson_t *bson)
{
   char *regex = NULL;
   char *options = NULL;

   if (!bson->bson_type_data.regex.has_regex) {
      _bson_json_read_set_error (reader,
                                 "Missing $regex after $options in BSON_TYPE_REGEX");
      return 0;
   }

   regex = (char *)bson->bson_type_buf[0].buf;

   if (bson->bson_type_data.regex.has_options) {
      options = (char *)bson->bson_type_buf[1].buf;
   }

   return bson_append_regex (STACK_BSON_CHILD, bson->key, bson->key_buf.len,
                             regex, options);
}

static int
_bson_json_read_append_oid (bson_json_reader_t       *reader,
                            _bson_json_reader_bson_t *bson)
{
   return bson_append_oid (STACK_BSON_CHILD, bson->key, bson->key_buf.len,
                           &bson->bson_type_data.oid.oid);
}

static int
_bson_json_read_append_date_time (bson_json_reader_t       *reader,
                                  _bson_json_reader_bson_t *bson)
{
   return bson_append_date_time (STACK_BSON_CHILD, bson->key, bson->key_buf.len,
                                 bson->bson_type_data.date.date);
}

static int
_bson_json_read_append_timestamp (bson_json_reader_t       *reader,
                                  _bson_json_reader_bson_t *bson)
{
   if (!bson->bson_type_data.timestamp.has_t) {
      _bson_json_read_set_error (reader,
                                 "Missing t after $timestamp in BSON_TYPE_TIMESTAMP");
      return 0;
   }

   if (!bson->bson_type_data.timestamp.has_i) {
      _bson_json_read_set_error (reader,
                                 "Missing i after $timestamp in BSON_TYPE_TIMESTAMP");
      return 0;
   }

   return bson_append_timestamp (STACK_BSON_CHILD, bson->key, bson->key_buf.len,
                                 bson->bson_type_data.timestamp.t,
                                 bson->bson_type_data.timestamp.i);
}

static int
_bson_json_read_append_dbpointer (bson_json_reader_t       *reader,
                                  _bson_json_reader_bson_t *bson)
{
   char *ref;

   if (!bson->bson_type_data.ref.has_ref) {
      _bson_json_read_set_error (reader,
                                 "Missing $ref after $id in BSON_TYPE_DBPOINTER");
      return 0;
   }

   if (!bson->bson_type_data.ref.has_id) {
      _bson_json_read_set_error (reader,
                                 "Missing $id after $ref in BSON_TYPE_DBPOINTER");
      return 0;
   }

   ref = (char *)bson->bson_type_buf[0].buf;

   return bson_append_dbpointer (STACK_BSON_CHILD, bson->key, bson->key_buf.len,
                                 ref, &bson->bson_type_data.ref.id);
}

static int
_bson_json_read_end_map (void *_ctx)
{
   bson_json_reader_t *reader = (bson_json_reader_t *)_ctx;
   _bson_json_reader_bson_t *bson = &reader->bson;

   if (bson->read_state == BSON_JSON_IN_START_MAP) {
      bson->read_state = BSON_JSON_REGULAR;
      STACK_PUSH_DOC (bson_append_document_begin (STACK_BSON_PARENT, bson->key,
                                                  bson->key_buf.len,
                                                  STACK_BSON_CHILD));
   }

   if (bson->read_state == BSON_JSON_IN_BSON_TYPE) {
      bson->read_state = BSON_JSON_REGULAR;
      switch (bson->bson_type) {
      case BSON_TYPE_REGEX:
         return _bson_json_read_append_regex (reader, bson);
      case BSON_TYPE_OID:
         return _bson_json_read_append_oid (reader, bson);
      case BSON_TYPE_BINARY:
         return _bson_json_read_append_binary (reader, bson);
      case BSON_TYPE_DATE_TIME:
         return _bson_json_read_append_date_time (reader, bson);
      case BSON_TYPE_DBPOINTER:
         return _bson_json_read_append_dbpointer (reader, bson);
      case BSON_TYPE_UNDEFINED:
         return bson_append_undefined (STACK_BSON_CHILD, bson->key,
                                       bson->key_buf.len);
      case BSON_TYPE_MINKEY:
         return bson_append_minkey (STACK_BSON_CHILD, bson->key,
                                    bson->key_buf.len);
      case BSON_TYPE_MAXKEY:
         return bson_append_maxkey (STACK_BSON_CHILD, bson->key,
                                    bson->key_buf.len);
      case BSON_TYPE_EOD:
      case BSON_TYPE_DOUBLE:
      case BSON_TYPE_UTF8:
      case BSON_TYPE_DOCUMENT:
      case BSON_TYPE_ARRAY:
      case BSON_TYPE_BOOL:
      case BSON_TYPE_NULL:
      case BSON_TYPE_CODE:
      case BSON_TYPE_SYMBOL:
      case BSON_TYPE_CODEWSCOPE:
      case BSON_TYPE_INT32:
      case BSON_TYPE_TIMESTAMP:
      case BSON_TYPE_INT64:
      default:
         _bson_json_read_set_error (reader, "Unknown type %d", bson->bson_type);
         return 0;
         break;
      }
   } else if (bson->read_state == BSON_JSON_IN_BSON_TYPE_TIMESTAMP_VALUES) {
      bson->read_state = BSON_JSON_IN_BSON_TYPE_TIMESTAMP_ENDMAP;

      return _bson_json_read_append_timestamp (reader, bson);
   } else if (bson->read_state == BSON_JSON_IN_BSON_TYPE_TIMESTAMP_ENDMAP) {
      bson->read_state = BSON_JSON_REGULAR;
   } else if (bson->read_state == BSON_JSON_REGULAR) {
      STACK_POP_DOC (bson_append_document_end (STACK_BSON_PARENT,
                                               STACK_BSON_CHILD));

      if (bson->n == -1) {
         bson->read_state = BSON_JSON_DONE;
         return 0;
      }
   } else {
      _bson_json_read_set_error (reader, "Invalid state %d", bson->read_state);
      return 0;
   }

   return 1;
}

static int
_bson_json_read_start_array (void *_ctx)
{
   BASIC_YAJL_CB_PREAMBLE;
   BASIC_YAJL_CB_BAIL_IF_NOT_NORMAL ("[");

   STACK_PUSH_ARRAY (bson_append_array_begin (STACK_BSON_PARENT, key, len,
                                              STACK_BSON_CHILD));

   return 1;
}

static int
_bson_json_read_end_array (void *_ctx)
{
   bson_json_reader_t *reader = (bson_json_reader_t *)_ctx;
   _bson_json_reader_bson_t *bson = &reader->bson;

   BASIC_YAJL_CB_BAIL_IF_NOT_NORMAL ("]");

   STACK_POP_ARRAY (bson_append_array_end (STACK_BSON_PARENT,
                                           STACK_BSON_CHILD));

   return 1;
}

static yajl_callbacks read_cbs = {
   _bson_json_read_null,
   _bson_json_read_boolean,
   _bson_json_read_integer,
   _bson_json_read_double,
   NULL,
   _bson_json_read_string,
   _bson_json_read_start_map,
   _bson_json_read_map_key,
   _bson_json_read_end_map,
   _bson_json_read_start_array,
   _bson_json_read_end_array
};

static int
_bson_json_read_parse_error (bson_json_reader_t *reader,
                             yajl_status         ys,
                             bson_error_t       *error)
{
   unsigned char *str;
   int r;
   yajl_handle yh = reader->yh;
   _bson_json_reader_bson_t *bson = &reader->bson;
   _bson_json_reader_producer_t *p = &reader->producer;

   if (ys == yajl_status_client_canceled) {
      if (bson->read_state == BSON_JSON_DONE) {
         r = 1;
      } else {
         r = -1;
      }
   } else {
      if (error) {
         str = yajl_get_error (yh, 1, p->buf + p->bytes_parsed,
                               p->bytes_read - p->bytes_parsed);
         bson_set_error (error, BSON_JSON_ERROR_READ,
                         BSON_JSON_ERROR_READ_CORRUPT_JS,
                         "%s", str);
         yajl_free_error (yh, str);
      }

      r = -1;
   }

   p->bytes_parsed += yajl_get_bytes_consumed (yh);

   yh->stateStack.used = 0;
   yajl_bs_push (yh->stateStack, yajl_state_start);

   return r;
}

int
bson_json_read (bson_json_reader_t *reader,
                bson_t             *bson,
                bson_error_t       *error)
{
   ssize_t r;
   yajl_status ys;
   bool read_something = false;
   _bson_json_reader_producer_t *p = &reader->producer;
   yajl_handle yh = reader->yh;

   reader->bson.bson = bson;
   reader->bson.n = -1;
   reader->bson.read_state = BSON_JSON_REGULAR;
   reader->error = error;

   for (;; ) {
      if (!read_something && p->bytes_parsed && p->bytes_read >
          p->bytes_parsed) {
         r = p->bytes_read - p->bytes_parsed;
      } else {
         r = p->cb (p->data, p->buf, p->buf_size);

         if (r) {
            p->bytes_read = r;
            p->bytes_parsed = 0;
         }
      }

      if (r < 0) {
         if (error) {
            bson_set_error (error, BSON_JSON_ERROR_READ,
                            BSON_JSON_ERROR_READ_CB_FAILURE,
                            "reader cb failed");
         }

         return -1;
      } else if (r == 0) {
         break;
      } else {
         read_something = true;

         ys = yajl_parse (yh, p->buf + p->bytes_parsed, r);

         if (ys != yajl_status_ok) {
            return _bson_json_read_parse_error (reader, ys, error);
         }
      }
   }

   if (read_something) {
      ys = yajl_complete_parse (yh);

      if (ys != yajl_status_ok) {
         return _bson_json_read_parse_error (reader, ys, error);
      }
   }

   return 0;
}

bson_json_reader_t *
bson_json_reader_new (void                *data,
                      bson_json_reader_cb  cb,
                      bson_json_destroy_cb dcb,
                      bool                 allow_multiple,
                      size_t               buf_size)
{
   bson_json_reader_t *r;
   _bson_json_reader_producer_t *p;

   r = calloc (sizeof *r, 1);

   p = &r->producer;

   p->data = data;
   p->cb = cb;
   p->dcb = dcb;
   p->buf = malloc (buf_size);
   p->buf_size = buf_size ? buf_size : BSON_JSON_DEFAULT_BUF_SIZE;

   r->yh = yajl_alloc (&read_cbs, NULL, r);

   yajl_config (r->yh,
                yajl_dont_validate_strings |
                (allow_multiple ?  yajl_allow_multiple_values : 0)
                , 1);

   return r;
}

void
bson_json_reader_destroy (bson_json_reader_t *reader)
{
   int i;
   _bson_json_reader_producer_t *p = &reader->producer;
   _bson_json_reader_bson_t *b = &reader->bson;

   if (reader->producer.dcb) {
      reader->producer.dcb (reader->producer.data);
   }

   free (p->buf);
   free (b->key_buf.buf);

   for (i = 0; i < 3; i++) {
      free (b->bson_type_buf[i].buf);
   }

   yajl_free (reader->yh);

   free (reader);
}

typedef struct
{
   const uint8_t *data;
   size_t         len;
   size_t         bytes_parsed;
} _bson_json_data_reader_t;

static ssize_t
_bson_json_data_reader_cb (void    *_ctx,
                           uint8_t *buf,
                           size_t   len)
{
   size_t bytes;
   _bson_json_data_reader_t *ctx = (_bson_json_data_reader_t *)_ctx;

   if (!ctx->data) {
      return -1;
   }

   bytes = MIN (len, ctx->len - ctx->bytes_parsed);

   memcpy (buf, ctx->data + ctx->bytes_parsed, bytes);

   ctx->bytes_parsed += bytes;

   return bytes;
}

bson_json_reader_t *
bson_json_data_reader_new (bool   allow_multiple,
                           size_t size)
{
   _bson_json_data_reader_t *dr = calloc (sizeof *dr, 1);

   return bson_json_reader_new (dr, &_bson_json_data_reader_cb, &free,
                                allow_multiple, size);
}

void
bson_json_data_reader_ingest (bson_json_reader_t *reader,
                              const uint8_t      *data,
                              size_t              len)
{
   _bson_json_data_reader_t *ctx =
      (_bson_json_data_reader_t *)reader->producer.data;

   ctx->data = data;
   ctx->len = len;
   ctx->bytes_parsed = 0;
}

bson_t *
bson_from_json (const uint8_t *data,
                size_t         len,
                bson_error_t  *error)
{
   bson_json_reader_t *reader = bson_json_data_reader_new (
      false, BSON_JSON_DEFAULT_BUF_SIZE);

   bson_t *bson = bson_new ();
   int r;

   bson_json_data_reader_ingest (reader, data, len);

   r = bson_json_read (reader, bson, error);

   bson_json_reader_destroy (reader);

   if (r == 1) {
      return bson;
   } else {
      bson_destroy (bson);
      return NULL;
   }
}
