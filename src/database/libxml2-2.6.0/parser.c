/*
 * parser.c : an XML 1.0 parser, namespaces and validity support are mostly
 *            implemented on top of the SAX interfaces
 *
 * References:
 *   The XML specification:
 *     http://www.w3.org/TR/REC-xml
 *   Original 1.0 version:
 *     http://www.w3.org/TR/1998/REC-xml-19980210
 *   XML second edition working draft
 *     http://www.w3.org/TR/2000/WD-xml-2e-20000814
 *
 * Okay this is a big file, the parser core is around 7000 lines, then it
 * is followed by the progressive parser top routines, then the various
 * high level APIs to call the parser and a few miscellaneous functions.
 * A number of helper functions and deprecated ones have been moved to
 * parserInternals.c to reduce this file size.
 * As much as possible the functions are associated with their relative
 * production in the XML specification. A few productions defining the
 * different ranges of character are actually implanted either in 
 * parserInternals.h or parserInternals.c
 * The DOM tree build is realized from the default SAX callbacks in
 * the module SAX.c.
 * The routines doing the validation checks are in valid.c and called either
 * from the SAX callbacks or as standalone functions using a preparsed
 * document.
 *
 * See Copyright for the status of this software.
 *
 * daniel@veillard.com
 */

#define IN_LIBXML
#include "libxml.h"

#if defined(WIN32) && !defined (__CYGWIN__)
#define XML_DIR_SEP '\\'
#else
#define XML_DIR_SEP '/'
#endif

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <libxml/xmlmemory.h>
#include <libxml/threads.h>
#include <libxml/globals.h>
#include <libxml/tree.h>
#include <libxml/parser.h>
#include <libxml/parserInternals.h>
#include <libxml/valid.h>
#include <libxml/entities.h>
#include <libxml/xmlerror.h>
#include <libxml/encoding.h>
#include <libxml/xmlIO.h>
#include <libxml/uri.h>
#ifdef LIBXML_CATALOG_ENABLED
#include <libxml/catalog.h>
#endif

#ifdef HAVE_CTYPE_H
#include <ctype.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_ZLIB_H
#include <zlib.h>
#endif

/**
 * xmlParserMaxDepth:
 *
 * arbitrary depth limit for the XML documents that we allow to 
 * process. This is not a limitation of the parser but a safety 
 * boundary feature.
 */
unsigned int xmlParserMaxDepth = 1024;

#define SAX2 1

#define XML_PARSER_BIG_BUFFER_SIZE 300
#define XML_PARSER_BUFFER_SIZE 100

#define SAX_COMPAT_MODE BAD_CAST "SAX compatibility mode document"

/*
 * List of XML prefixed PI allowed by W3C specs
 */

static const char *xmlW3CPIs[] = {
    "xml-stylesheet",
    NULL
};


/* DEPR void xmlParserHandleReference(xmlParserCtxtPtr ctxt); */
xmlEntityPtr xmlParseStringPEReference(xmlParserCtxtPtr ctxt,
                                       const xmlChar **str);

static xmlParserErrors
xmlParseExternalEntityPrivate(xmlDocPtr doc, xmlParserCtxtPtr oldctxt,
	              xmlSAXHandlerPtr sax,
		      void *user_data, int depth, const xmlChar *URL,
		      const xmlChar *ID, xmlNodePtr *list);

#ifdef LIBXML_LEGACY_ENABLED
static void
xmlAddEntityReference(xmlEntityPtr ent, xmlNodePtr firstNode,
                      xmlNodePtr lastNode);
#endif /* LIBXML_LEGACY_ENABLED */

static xmlParserErrors
xmlParseBalancedChunkMemoryInternal(xmlParserCtxtPtr oldctxt,
		      const xmlChar *string, void *user_data, xmlNodePtr *lst);

/************************************************************************
 *									*
 * 		Some factorized error routines				*
 *									*
 ************************************************************************/

/**
 * xmlErrAttributeDup:
 * @ctxt:  an XML parser context
 * @prefix:  the attribute prefix
 * @localname:  the attribute localname
 *
 * Handle a redefinition of attribute error
 */
static void
xmlErrAttributeDup(xmlParserCtxtPtr ctxt, const xmlChar * prefix,
                   const xmlChar * localname)
{
    ctxt->errNo = XML_ERR_ATTRIBUTE_REDEFINED;
    if (prefix == NULL)
        __xmlRaiseError(NULL, NULL, NULL, ctxt, NULL, XML_FROM_PARSER,
                        ctxt->errNo, XML_ERR_FATAL, NULL, 0,
                        (const char *) localname, NULL, NULL, 0, 0,
                        "Attribute %s redefined\n", localname);
    else
        __xmlRaiseError(NULL, NULL, NULL, ctxt, NULL, XML_FROM_PARSER,
                        ctxt->errNo, XML_ERR_FATAL, NULL, 0,
                        (const char *) prefix, (const char *) localname,
                        NULL, 0, 0, "Attribute %s:%s redefined\n", prefix,
                        localname);
    ctxt->wellFormed = 0;
    if (ctxt->recovery == 0)
        ctxt->disableSAX = 1;
}

/**
 * xmlFatalErr:
 * @ctxt:  an XML parser context
 * @error:  the error number
 * @extra:  extra information string
 *
 * Handle a fatal parser error, i.e. violating Well-Formedness constraints
 */
static void
xmlFatalErr(xmlParserCtxtPtr ctxt, xmlParserErrors error, const char *info)
{
    const char *errmsg;

    switch (error) {
        case XML_ERR_INVALID_HEX_CHARREF:
            errmsg = "CharRef: invalid hexadecimal value\n";
            break;
        case XML_ERR_INVALID_DEC_CHARREF:
            errmsg = "CharRef: invalid decimal value\n";
            break;
        case XML_ERR_INVALID_CHARREF:
            errmsg = "CharRef: invalid value\n";
            break;
        case XML_ERR_INTERNAL_ERROR:
            errmsg = "internal error";
            break;
        case XML_ERR_PEREF_AT_EOF:
            errmsg = "PEReference at end of document\n";
            break;
        case XML_ERR_PEREF_IN_PROLOG:
            errmsg = "PEReference in prolog\n";
            break;
        case XML_ERR_PEREF_IN_EPILOG:
            errmsg = "PEReference in epilog\n";
            break;
        case XML_ERR_PEREF_NO_NAME:
            errmsg = "PEReference: no name\n";
            break;
        case XML_ERR_PEREF_SEMICOL_MISSING:
            errmsg = "PEReference: expecting ';'\n";
            break;
        case XML_ERR_ENTITY_LOOP:
            errmsg = "Detected an entity reference loop\n";
            break;
        case XML_ERR_ENTITY_NOT_STARTED:
            errmsg = "EntityValue: \" or ' expected\n";
            break;
        case XML_ERR_ENTITY_PE_INTERNAL:
            errmsg = "PEReferences forbidden in internal subset\n";
            break;
        case XML_ERR_ENTITY_NOT_FINISHED:
            errmsg = "EntityValue: \" or ' expected\n";
            break;
        case XML_ERR_ATTRIBUTE_NOT_STARTED:
            errmsg = "AttValue: \" or ' expected\n";
            break;
        case XML_ERR_LT_IN_ATTRIBUTE:
            errmsg = "Unescaped '<' not allowed in attributes values\n";
            break;
        case XML_ERR_LITERAL_NOT_STARTED:
            errmsg = "SystemLiteral \" or ' expected\n";
            break;
        case XML_ERR_LITERAL_NOT_FINISHED:
            errmsg = "Unfinished System or Public ID \" or ' expected\n";
            break;
        case XML_ERR_MISPLACED_CDATA_END:
            errmsg = "Sequence ']]>' not allowed in content\n";
            break;
        case XML_ERR_URI_REQUIRED:
            errmsg = "SYSTEM or PUBLIC, the URI is missing\n";
            break;
        case XML_ERR_PUBID_REQUIRED:
            errmsg = "PUBLIC, the Public Identifier is missing\n";
            break;
        case XML_ERR_HYPHEN_IN_COMMENT:
            errmsg = "Comment must not contain '--' (double-hyphen)\n";
            break;
        case XML_ERR_PI_NOT_STARTED:
            errmsg = "xmlParsePI : no target name\n";
            break;
        case XML_ERR_RESERVED_XML_NAME:
            errmsg = "Invalid PI name\n";
            break;
        case XML_ERR_NOTATION_NOT_STARTED:
            errmsg = "NOTATION: Name expected here\n";
            break;
        case XML_ERR_NOTATION_NOT_FINISHED:
            errmsg = "'>' required to close NOTATION declaration\n";
            break;
        case XML_ERR_VALUE_REQUIRED:
            errmsg = "Entity value required\n";
            break;
        case XML_ERR_URI_FRAGMENT:
            errmsg = "Fragment not allowed";
            break;
        case XML_ERR_ATTLIST_NOT_STARTED:
            errmsg = "'(' required to start ATTLIST enumeration\n";
            break;
        case XML_ERR_NMTOKEN_REQUIRED:
            errmsg = "NmToken expected in ATTLIST enumeration\n";
            break;
        case XML_ERR_ATTLIST_NOT_FINISHED:
            errmsg = "')' required to finish ATTLIST enumeration\n";
            break;
        case XML_ERR_MIXED_NOT_STARTED:
            errmsg = "MixedContentDecl : '|' or ')*' expected\n";
            break;
        case XML_ERR_PCDATA_REQUIRED:
            errmsg = "MixedContentDecl : '#PCDATA' expected\n";
            break;
        case XML_ERR_ELEMCONTENT_NOT_STARTED:
            errmsg = "ContentDecl : Name or '(' expected\n";
            break;
        case XML_ERR_ELEMCONTENT_NOT_FINISHED:
            errmsg = "ContentDecl : ',' '|' or ')' expected\n";
            break;
        case XML_ERR_PEREF_IN_INT_SUBSET:
            errmsg =
                "PEReference: forbidden within markup decl in internal subset\n";
            break;
        case XML_ERR_GT_REQUIRED:
            errmsg = "expected '>'\n";
            break;
        case XML_ERR_CONDSEC_INVALID:
            errmsg = "XML conditional section '[' expected\n";
            break;
        case XML_ERR_EXT_SUBSET_NOT_FINISHED:
            errmsg = "Content error in the external subset\n";
            break;
        case XML_ERR_CONDSEC_INVALID_KEYWORD:
            errmsg =
                "conditional section INCLUDE or IGNORE keyword expected\n";
            break;
        case XML_ERR_CONDSEC_NOT_FINISHED:
            errmsg = "XML conditional section not closed\n";
            break;
        case XML_ERR_XMLDECL_NOT_STARTED:
            errmsg = "Text declaration '<?xml' required\n";
            break;
        case XML_ERR_XMLDECL_NOT_FINISHED:
            errmsg = "parsing XML declaration: '?>' expected\n";
            break;
        case XML_ERR_EXT_ENTITY_STANDALONE:
            errmsg = "external parsed entities cannot be standalone\n";
            break;
        case XML_ERR_ENTITYREF_SEMICOL_MISSING:
            errmsg = "EntityRef: expecting ';'\n";
            break;
        case XML_ERR_DOCTYPE_NOT_FINISHED:
            errmsg = "DOCTYPE improperly terminated\n";
            break;
        case XML_ERR_LTSLASH_REQUIRED:
            errmsg = "EndTag: '</' not found\n";
            break;
        case XML_ERR_EQUAL_REQUIRED:
            errmsg = "expected '='\n";
            break;
        case XML_ERR_STRING_NOT_CLOSED:
            errmsg = "String not closed expecting \" or '\n";
            break;
        case XML_ERR_STRING_NOT_STARTED:
            errmsg = "String not started expecting ' or \"\n";
            break;
        case XML_ERR_ENCODING_NAME:
            errmsg = "Invalid XML encoding name\n";
            break;
        case XML_ERR_STANDALONE_VALUE:
            errmsg = "standalone accepts only 'yes' or 'no'\n";
            break;
        case XML_ERR_DOCUMENT_EMPTY:
            errmsg = "Document is empty\n";
            break;
        case XML_ERR_DOCUMENT_END:
            errmsg = "Extra content at the end of the document\n";
            break;
        case XML_ERR_NOT_WELL_BALANCED:
            errmsg = "chunk is not well balanced\n";
            break;
        case XML_ERR_EXTRA_CONTENT:
            errmsg = "extra content at the end of well balanced chunk\n";
            break;
        case XML_ERR_VERSION_MISSING:
            errmsg = "Malformed declaration expecting version\n";
            break;
#if 0
        case:
            errmsg = "\n";
            break;
#endif
        default:
            errmsg = "Unregistered error message\n";
    }
    ctxt->errNo = error;
    __xmlRaiseError(NULL, NULL, NULL, ctxt, NULL, XML_FROM_PARSER, error,
                    XML_ERR_FATAL, NULL, 0, info, NULL, NULL, 0, 0, errmsg,
                    info);
    ctxt->wellFormed = 0;
    if (ctxt->recovery == 0)
        ctxt->disableSAX = 1;
}

/**
 * xmlFatalErrMsg:
 * @ctxt:  an XML parser context
 * @error:  the error number
 * @msg:  the error message
 *
 * Handle a fatal parser error, i.e. violating Well-Formedness constraints
 */
static void
xmlFatalErrMsg(xmlParserCtxtPtr ctxt, xmlParserErrors error,
               const char *msg)
{
    ctxt->errNo = error;
    __xmlRaiseError(NULL, NULL, NULL, ctxt, NULL, XML_FROM_PARSER, error,
                    XML_ERR_FATAL, NULL, 0, NULL, NULL, NULL, 0, 0, msg);
    ctxt->wellFormed = 0;
    if (ctxt->recovery == 0)
        ctxt->disableSAX = 1;
}

/**
 * xmlWarningMsg:
 * @ctxt:  an XML parser context
 * @error:  the error number
 * @msg:  the error message
 * @str1:  extra data
 * @str2:  extra data
 *
 * Handle a warning.
 */
static void
xmlWarningMsg(xmlParserCtxtPtr ctxt, xmlParserErrors error,
              const char *msg, const xmlChar *str1, const xmlChar *str2)
{
    xmlStructuredErrorFunc schannel = NULL;
    
    ctxt->errNo = error;
    if ((ctxt->sax != NULL) && (ctxt->sax->initialized == XML_SAX2_MAGIC))
        schannel = ctxt->sax->serror;
    __xmlRaiseError(schannel,
                    (ctxt->sax) ? ctxt->sax->warning : NULL,
                    ctxt->userData,
                    ctxt, NULL, XML_FROM_PARSER, error,
                    XML_ERR_WARNING, NULL, 0,
		    (const char *) str1, (const char *) str2, NULL, 0, 0,
		    msg, (const char *) str1, (const char *) str2);
}

/**
 * xmlValidityError:
 * @ctxt:  an XML parser context
 * @error:  the error number
 * @msg:  the error message
 * @str1:  extra data
 *
 * Handle a warning.
 */
static void
xmlValidityError(xmlParserCtxtPtr ctxt, xmlParserErrors error,
              const char *msg, const xmlChar *str1)
{
    xmlStructuredErrorFunc schannel = NULL;
    ctxt->errNo = error;
    if ((ctxt->sax != NULL) && (ctxt->sax->initialized == XML_SAX2_MAGIC))
        schannel = ctxt->sax->serror;
    __xmlRaiseError(schannel,
                    ctxt->vctxt.error, ctxt->vctxt.userData,
                    ctxt, NULL, XML_FROM_DTD, error,
                    XML_ERR_ERROR, NULL, 0, (const char *) str1,
		    NULL, NULL, 0, 0,
		    msg, (const char *) str1);
    ctxt->valid = 0;
}

/**
 * xmlFatalErrMsgInt:
 * @ctxt:  an XML parser context
 * @error:  the error number
 * @msg:  the error message
 * @val:  an integer value
 *
 * Handle a fatal parser error, i.e. violating Well-Formedness constraints
 */
static void
xmlFatalErrMsgInt(xmlParserCtxtPtr ctxt, xmlParserErrors error,
                  const char *msg, int val)
{
    ctxt->errNo = error;
    __xmlRaiseError(NULL, NULL, NULL,
                    ctxt, NULL, XML_FROM_PARSER, error, XML_ERR_FATAL,
                    NULL, 0, NULL, NULL, NULL, val, 0, msg, val);
    ctxt->wellFormed = 0;
    if (ctxt->recovery == 0)
        ctxt->disableSAX = 1;
}

/**
 * xmlFatalErrMsgStrIntStr:
 * @ctxt:  an XML parser context
 * @error:  the error number
 * @msg:  the error message
 * @str1:  an string info
 * @val:  an integer value
 * @str2:  an string info
 *
 * Handle a fatal parser error, i.e. violating Well-Formedness constraints
 */
static void
xmlFatalErrMsgStrIntStr(xmlParserCtxtPtr ctxt, xmlParserErrors error,
                  const char *msg, const xmlChar *str1, int val, 
		  const xmlChar *str2)
{
    ctxt->errNo = error;
    __xmlRaiseError(NULL, NULL, NULL,
                    ctxt, NULL, XML_FROM_PARSER, error, XML_ERR_FATAL,
                    NULL, 0, (const char *) str1, (const char *) str2,
		    NULL, val, 0, msg, str1, val, str2);
    ctxt->wellFormed = 0;
    if (ctxt->recovery == 0)
        ctxt->disableSAX = 1;
}

/**
 * xmlFatalErrMsgStr:
 * @ctxt:  an XML parser context
 * @error:  the error number
 * @msg:  the error message
 * @val:  a string value
 *
 * Handle a fatal parser error, i.e. violating Well-Formedness constraints
 */
static void
xmlFatalErrMsgStr(xmlParserCtxtPtr ctxt, xmlParserErrors error,
                  const char *msg, const xmlChar * val)
{
    ctxt->errNo = error;
    __xmlRaiseError(NULL, NULL, NULL, ctxt, NULL,
                    XML_FROM_PARSER, error, XML_ERR_FATAL,
                    NULL, 0, (const char *) val, NULL, NULL, 0, 0, msg,
                    val);
    ctxt->wellFormed = 0;
    if (ctxt->recovery == 0)
        ctxt->disableSAX = 1;
}

/**
 * xmlErrMsgStr:
 * @ctxt:  an XML parser context
 * @error:  the error number
 * @msg:  the error message
 * @val:  a string value
 *
 * Handle a non fatal parser error
 */
static void
xmlErrMsgStr(xmlParserCtxtPtr ctxt, xmlParserErrors error,
                  const char *msg, const xmlChar * val)
{
    ctxt->errNo = error;
    __xmlRaiseError(NULL, NULL, NULL, ctxt, NULL,
                    XML_FROM_PARSER, error, XML_ERR_ERROR,
                    NULL, 0, (const char *) val, NULL, NULL, 0, 0, msg,
                    val);
}

/**
 * xmlNsErr:
 * @ctxt:  an XML parser context
 * @error:  the error number
 * @msg:  the message
 * @info1:  extra information string
 * @info2:  extra information string
 *
 * Handle a fatal parser error, i.e. violating Well-Formedness constraints
 */
static void
xmlNsErr(xmlParserCtxtPtr ctxt, xmlParserErrors error,
         const char *msg,
         const xmlChar * info1, const xmlChar * info2,
         const xmlChar * info3)
{
    ctxt->errNo = error;
    __xmlRaiseError(NULL, NULL, NULL, ctxt, NULL, XML_FROM_NAMESPACE, error,
                    XML_ERR_ERROR, NULL, 0, (const char *) info1,
                    (const char *) info2, (const char *) info3, 0, 0, msg,
                    info1, info2, info3);
    ctxt->nsWellFormed = 0;
}

/************************************************************************
 *									*
 * 		SAX2 defaulted attributes handling			*
 *									*
 ************************************************************************/

/**
 * xmlDetectSAX2:
 * @ctxt:  an XML parser context
 *
 * Do the SAX2 detection and specific intialization
 */
static void
xmlDetectSAX2(xmlParserCtxtPtr ctxt) {
    if (ctxt == NULL) return;
#ifdef LIBXML_SAX1_ENABLED
    if ((ctxt->sax) &&  (ctxt->sax->initialized == XML_SAX2_MAGIC) &&
        ((ctxt->sax->startElementNs != NULL) ||
         (ctxt->sax->endElementNs != NULL))) ctxt->sax2 = 1;
#else
    ctxt->sax2 = 1;
#endif /* LIBXML_SAX1_ENABLED */

    ctxt->str_xml = xmlDictLookup(ctxt->dict, BAD_CAST "xml", 3);
    ctxt->str_xmlns = xmlDictLookup(ctxt->dict, BAD_CAST "xmlns", 5);
    ctxt->str_xml_ns = xmlDictLookup(ctxt->dict, XML_XML_NAMESPACE, 36);
}

typedef struct _xmlDefAttrs xmlDefAttrs;
typedef xmlDefAttrs *xmlDefAttrsPtr;
struct _xmlDefAttrs {
    int nbAttrs;	/* number of defaulted attributes on that element */
    int maxAttrs;       /* the size of the array */
    const xmlChar *values[4]; /* array of localname/prefix/values */
};

/**
 * xmlAddDefAttrs:
 * @ctxt:  an XML parser context
 * @fullname:  the element fullname
 * @fullattr:  the attribute fullname
 * @value:  the attribute value
 *
 * Add a defaulted attribute for an element
 */
static void
xmlAddDefAttrs(xmlParserCtxtPtr ctxt,
               const xmlChar *fullname,
               const xmlChar *fullattr,
               const xmlChar *value) {
    xmlDefAttrsPtr defaults;
    int len;
    const xmlChar *name;
    const xmlChar *prefix;

    if (ctxt->attsDefault == NULL) {
        ctxt->attsDefault = xmlHashCreate(10);
	if (ctxt->attsDefault == NULL)
	    goto mem_error;
    }

    /*
     * plit the element name into prefix:localname , the string found
     * are within the DTD and hen not associated to namespace names.
     */
    name = xmlSplitQName3(fullname, &len);
    if (name == NULL) {
        name = xmlDictLookup(ctxt->dict, fullname, -1);
	prefix = NULL;
    } else {
        name = xmlDictLookup(ctxt->dict, name, -1);
	prefix = xmlDictLookup(ctxt->dict, fullname, len);
    }

    /*
     * make sure there is some storage
     */
    defaults = xmlHashLookup2(ctxt->attsDefault, name, prefix);
    if (defaults == NULL) {
        defaults = (xmlDefAttrsPtr) xmlMalloc(sizeof(xmlDefAttrs) +
	                               12 * sizeof(const xmlChar *));
	if (defaults == NULL)
	    goto mem_error;
	defaults->maxAttrs = 4;
	defaults->nbAttrs = 0;
	xmlHashUpdateEntry2(ctxt->attsDefault, name, prefix, defaults, NULL);
    } else if (defaults->nbAttrs >= defaults->maxAttrs) {
        defaults = (xmlDefAttrsPtr) xmlRealloc(defaults, sizeof(xmlDefAttrs) +
		       (2 * defaults->maxAttrs * 4) * sizeof(const xmlChar *));
	if (defaults == NULL)
	    goto mem_error;
	defaults->maxAttrs *= 2;
	xmlHashUpdateEntry2(ctxt->attsDefault, name, prefix, defaults, NULL);
    }

    /*
     * plit the element name into prefix:localname , the string found
     * are within the DTD and hen not associated to namespace names.
     */
    name = xmlSplitQName3(fullattr, &len);
    if (name == NULL) {
        name = xmlDictLookup(ctxt->dict, fullattr, -1);
	prefix = NULL;
    } else {
        name = xmlDictLookup(ctxt->dict, name, -1);
	prefix = xmlDictLookup(ctxt->dict, fullattr, len);
    }

    defaults->values[4 * defaults->nbAttrs] = name;
    defaults->values[4 * defaults->nbAttrs + 1] = prefix;
    /* intern the string and precompute the end */
    len = xmlStrlen(value);
    value = xmlDictLookup(ctxt->dict, value, len);
    defaults->values[4 * defaults->nbAttrs + 2] = value;
    defaults->values[4 * defaults->nbAttrs + 3] = value + len;
    defaults->nbAttrs++;

    return;

mem_error:
    xmlErrMemory(ctxt, NULL);
    return;
}

/**
 * xmlAddSpecialAttr:
 * @ctxt:  an XML parser context
 * @fullname:  the element fullname
 * @fullattr:  the attribute fullname
 * @type:  the attribute type
 *
 * Register that this attribute is not CDATA
 */
static void
xmlAddSpecialAttr(xmlParserCtxtPtr ctxt,
		  const xmlChar *fullname,
		  const xmlChar *fullattr,
		  int type)
{
    if (ctxt->attsSpecial == NULL) {
        ctxt->attsSpecial = xmlHashCreate(10);
	if (ctxt->attsSpecial == NULL)
	    goto mem_error;
    }

    xmlHashAddEntry2(ctxt->attsSpecial, fullname, fullattr,
                     (void *) (long) type);
    return;

mem_error:
    xmlErrMemory(ctxt, NULL);
    return;
}

/**
 * xmlCheckLanguageID:
 * @lang:  pointer to the string value
 *
 * Checks that the value conforms to the LanguageID production:
 *
 * NOTE: this is somewhat deprecated, those productions were removed from
 *       the XML Second edition.
 *
 * [33] LanguageID ::= Langcode ('-' Subcode)*
 * [34] Langcode ::= ISO639Code |  IanaCode |  UserCode
 * [35] ISO639Code ::= ([a-z] | [A-Z]) ([a-z] | [A-Z])
 * [36] IanaCode ::= ('i' | 'I') '-' ([a-z] | [A-Z])+
 * [37] UserCode ::= ('x' | 'X') '-' ([a-z] | [A-Z])+
 * [38] Subcode ::= ([a-z] | [A-Z])+
 *
 * Returns 1 if correct 0 otherwise
 **/
int
xmlCheckLanguageID(const xmlChar * lang)
{
    const xmlChar *cur = lang;

    if (cur == NULL)
        return (0);
    if (((cur[0] == 'i') && (cur[1] == '-')) ||
        ((cur[0] == 'I') && (cur[1] == '-'))) {
        /*
         * IANA code
         */
        cur += 2;
        while (((cur[0] >= 'A') && (cur[0] <= 'Z')) ||  /* non input consuming */
               ((cur[0] >= 'a') && (cur[0] <= 'z')))
            cur++;
    } else if (((cur[0] == 'x') && (cur[1] == '-')) ||
               ((cur[0] == 'X') && (cur[1] == '-'))) {
        /*
         * User code
         */
        cur += 2;
        while (((cur[0] >= 'A') && (cur[0] <= 'Z')) ||  /* non input consuming */
               ((cur[0] >= 'a') && (cur[0] <= 'z')))
            cur++;
    } else if (((cur[0] >= 'A') && (cur[0] <= 'Z')) ||
               ((cur[0] >= 'a') && (cur[0] <= 'z'))) {
        /*
         * ISO639
         */
        cur++;
        if (((cur[0] >= 'A') && (cur[0] <= 'Z')) ||
            ((cur[0] >= 'a') && (cur[0] <= 'z')))
            cur++;
        else
            return (0);
    } else
        return (0);
    while (cur[0] != 0) {       /* non input consuming */
        if (cur[0] != '-')
            return (0);
        cur++;
        if (((cur[0] >= 'A') && (cur[0] <= 'Z')) ||
            ((cur[0] >= 'a') && (cur[0] <= 'z')))
            cur++;
        else
            return (0);
        while (((cur[0] >= 'A') && (cur[0] <= 'Z')) ||  /* non input consuming */
               ((cur[0] >= 'a') && (cur[0] <= 'z')))
            cur++;
    }
    return (1);
}

/************************************************************************
 *									*
 * 		Parser stacks related functions and macros		*
 *									*
 ************************************************************************/

xmlEntityPtr xmlParseStringEntityRef(xmlParserCtxtPtr ctxt,
                                     const xmlChar ** str);

#ifdef SAX2
/**
 * nsPush:
 * @ctxt:  an XML parser context
 * @prefix:  the namespace prefix or NULL
 * @URL:  the namespace name
 *
 * Pushes a new parser namespace on top of the ns stack
 *
 * Returns -1 in case of error, -2 if the namespace should be discarded
 *	   and the index in the stack otherwise.
 */
static int
nsPush(xmlParserCtxtPtr ctxt, const xmlChar *prefix, const xmlChar *URL)
{
    if (ctxt->options & XML_PARSE_NSCLEAN) {
        int i;
	for (i = 0;i < ctxt->nsNr;i += 2) {
	    if (ctxt->nsTab[i] == prefix) {
		/* in scope */
	        if (ctxt->nsTab[i + 1] == URL)
		    return(-2);
		/* out of scope keep it */
		break;
	    }
	}
    }
    if ((ctxt->nsMax == 0) || (ctxt->nsTab == NULL)) {
	ctxt->nsMax = 10;
	ctxt->nsNr = 0;
	ctxt->nsTab = (const xmlChar **)
	              xmlMalloc(ctxt->nsMax * sizeof(xmlChar *));
	if (ctxt->nsTab == NULL) {
	    xmlErrMemory(ctxt, NULL);
	    ctxt->nsMax = 0;
            return (-1);
	}
    } else if (ctxt->nsNr >= ctxt->nsMax) {
        ctxt->nsMax *= 2;
        ctxt->nsTab = (const xmlChar **)
	              xmlRealloc(ctxt->nsTab,
				 ctxt->nsMax * sizeof(ctxt->nsTab[0]));
        if (ctxt->nsTab == NULL) {
            xmlErrMemory(ctxt, NULL);
	    ctxt->nsMax /= 2;
            return (-1);
        }
    }
    ctxt->nsTab[ctxt->nsNr++] = prefix;
    ctxt->nsTab[ctxt->nsNr++] = URL;
    return (ctxt->nsNr);
}
/**
 * nsPop:
 * @ctxt: an XML parser context
 * @nr:  the number to pop
 *
 * Pops the top @nr parser prefix/namespace from the ns stack
 *
 * Returns the number of namespaces removed
 */
static int
nsPop(xmlParserCtxtPtr ctxt, int nr)
{
    int i;

    if (ctxt->nsTab == NULL) return(0);
    if (ctxt->nsNr < nr) {
        xmlGenericError(xmlGenericErrorContext, "Pbm popping %d NS\n", nr);
        nr = ctxt->nsNr;
    }
    if (ctxt->nsNr <= 0)
        return (0);
    
    for (i = 0;i < nr;i++) {
         ctxt->nsNr--;
	 ctxt->nsTab[ctxt->nsNr] = NULL;
    }
    return(nr);
}
#endif

static int
xmlCtxtGrowAttrs(xmlParserCtxtPtr ctxt, int nr) {
    const xmlChar **atts;
    int *attallocs;
    int maxatts;

    if (ctxt->atts == NULL) {
	maxatts = 55; /* allow for 10 attrs by default */
	atts = (const xmlChar **)
	       xmlMalloc(maxatts * sizeof(xmlChar *));
	if (atts == NULL) goto mem_error;
	ctxt->atts = atts;
	attallocs = (int *) xmlMalloc((maxatts / 5) * sizeof(int));
	if (attallocs == NULL) goto mem_error;
	ctxt->attallocs = attallocs;
	ctxt->maxatts = maxatts;
    } else if (nr + 5 > ctxt->maxatts) {
	maxatts = (nr + 5) * 2;
	atts = (const xmlChar **) xmlRealloc((void *) ctxt->atts,
				     maxatts * sizeof(const xmlChar *));
	if (atts == NULL) goto mem_error;
	ctxt->atts = atts;
	attallocs = (int *) xmlRealloc((void *) ctxt->attallocs,
	                             (maxatts / 5) * sizeof(int));
	if (attallocs == NULL) goto mem_error;
	ctxt->attallocs = attallocs;
	ctxt->maxatts = maxatts;
    }
    return(ctxt->maxatts);
mem_error:
    xmlErrMemory(ctxt, NULL);
    return(-1);
}

/**
 * inputPush:
 * @ctxt:  an XML parser context
 * @value:  the parser input
 *
 * Pushes a new parser input on top of the input stack
 *
 * Returns 0 in case of error, the index in the stack otherwise
 */
extern int
inputPush(xmlParserCtxtPtr ctxt, xmlParserInputPtr value)
{
    if (ctxt->inputNr >= ctxt->inputMax) {
        ctxt->inputMax *= 2;
        ctxt->inputTab =
            (xmlParserInputPtr *) xmlRealloc(ctxt->inputTab,
                                             ctxt->inputMax *
                                             sizeof(ctxt->inputTab[0]));
        if (ctxt->inputTab == NULL) {
            xmlErrMemory(ctxt, NULL);
            return (0);
        }
    }
    ctxt->inputTab[ctxt->inputNr] = value;
    ctxt->input = value;
    return (ctxt->inputNr++);
}
/**
 * inputPop:
 * @ctxt: an XML parser context
 *
 * Pops the top parser input from the input stack
 *
 * Returns the input just removed
 */
extern xmlParserInputPtr
inputPop(xmlParserCtxtPtr ctxt)
{
    xmlParserInputPtr ret;

    if (ctxt->inputNr <= 0)
        return (0);
    ctxt->inputNr--;
    if (ctxt->inputNr > 0)
        ctxt->input = ctxt->inputTab[ctxt->inputNr - 1];
    else
        ctxt->input = NULL;
    ret = ctxt->inputTab[ctxt->inputNr];
    ctxt->inputTab[ctxt->inputNr] = 0;
    return (ret);
}
/**
 * nodePush:
 * @ctxt:  an XML parser context
 * @value:  the element node
 *
 * Pushes a new element node on top of the node stack
 *
 * Returns 0 in case of error, the index in the stack otherwise
 */
extern int
nodePush(xmlParserCtxtPtr ctxt, xmlNodePtr value)
{
    if (ctxt->nodeNr >= ctxt->nodeMax) {
        ctxt->nodeMax *= 2;
        ctxt->nodeTab =
            (xmlNodePtr *) xmlRealloc(ctxt->nodeTab,
                                      ctxt->nodeMax *
                                      sizeof(ctxt->nodeTab[0]));
        if (ctxt->nodeTab == NULL) {
            xmlErrMemory(ctxt, NULL);
            return (0);
        }
    }
    if (((unsigned int) ctxt->nodeNr) > xmlParserMaxDepth) {
	xmlFatalErrMsgInt(ctxt, XML_ERR_INTERNAL_ERROR,
		 "Excessive depth in document: change xmlParserMaxDepth = %d\n",
			  xmlParserMaxDepth);
	ctxt->instate = XML_PARSER_EOF;
	return(0);
    }
    ctxt->nodeTab[ctxt->nodeNr] = value;
    ctxt->node = value;
    return (ctxt->nodeNr++);
}
/**
 * nodePop:
 * @ctxt: an XML parser context
 *
 * Pops the top element node from the node stack
 *
 * Returns the node just removed
 */
extern xmlNodePtr
nodePop(xmlParserCtxtPtr ctxt)
{
    xmlNodePtr ret;

    if (ctxt->nodeNr <= 0)
        return (0);
    ctxt->nodeNr--;
    if (ctxt->nodeNr > 0)
        ctxt->node = ctxt->nodeTab[ctxt->nodeNr - 1];
    else
        ctxt->node = NULL;
    ret = ctxt->nodeTab[ctxt->nodeNr];
    ctxt->nodeTab[ctxt->nodeNr] = 0;
    return (ret);
}
/**
 * nameNsPush:
 * @ctxt:  an XML parser context
 * @value:  the element name
 * @prefix:  the element prefix
 * @URI:  the element namespace name
 *
 * Pushes a new element name/prefix/URL on top of the name stack
 *
 * Returns -1 in case of error, the index in the stack otherwise
 */
static int
nameNsPush(xmlParserCtxtPtr ctxt, const xmlChar * value,
           const xmlChar *prefix, const xmlChar *URI, int nsNr)
{
    if (ctxt->nameNr >= ctxt->nameMax) {
        const xmlChar * *tmp;
        void **tmp2;
        ctxt->nameMax *= 2;
        tmp = (const xmlChar * *) xmlRealloc((xmlChar * *)ctxt->nameTab,
                                    ctxt->nameMax *
                                    sizeof(ctxt->nameTab[0]));
        if (tmp == NULL) {
	    ctxt->nameMax /= 2;
	    goto mem_error;
        }
	ctxt->nameTab = tmp;
        tmp2 = (void **) xmlRealloc((void * *)ctxt->pushTab,
                                    ctxt->nameMax * 3 *
                                    sizeof(ctxt->pushTab[0]));
        if (tmp2 == NULL) {
	    ctxt->nameMax /= 2;
	    goto mem_error;
        }
	ctxt->pushTab = tmp2;
    }
    ctxt->nameTab[ctxt->nameNr] = value;
    ctxt->name = value;
    ctxt->pushTab[ctxt->nameNr * 3] = (void *) prefix;
    ctxt->pushTab[ctxt->nameNr * 3 + 1] = (void *) URI;
    ctxt->pushTab[ctxt->nameNr * 3 + 2] = (void *) (long) nsNr;
    return (ctxt->nameNr++);
mem_error:
    xmlErrMemory(ctxt, NULL);
    return (-1);
}
/**
 * nameNsPop:
 * @ctxt: an XML parser context
 *
 * Pops the top element/prefix/URI name from the name stack
 *
 * Returns the name just removed
 */
static const xmlChar *
nameNsPop(xmlParserCtxtPtr ctxt)
{
    const xmlChar *ret;

    if (ctxt->nameNr <= 0)
        return (0);
    ctxt->nameNr--;
    if (ctxt->nameNr > 0)
        ctxt->name = ctxt->nameTab[ctxt->nameNr - 1];
    else
        ctxt->name = NULL;
    ret = ctxt->nameTab[ctxt->nameNr];
    ctxt->nameTab[ctxt->nameNr] = NULL;
    return (ret);
}

/**
 * namePush:
 * @ctxt:  an XML parser context
 * @value:  the element name
 *
 * Pushes a new element name on top of the name stack
 *
 * Returns -1 in case of error, the index in the stack otherwise
 */
extern int
namePush(xmlParserCtxtPtr ctxt, const xmlChar * value)
{
    if (ctxt->nameNr >= ctxt->nameMax) {
        const xmlChar * *tmp;
        ctxt->nameMax *= 2;
        tmp = (const xmlChar * *) xmlRealloc((xmlChar * *)ctxt->nameTab,
                                    ctxt->nameMax *
                                    sizeof(ctxt->nameTab[0]));
        if (tmp == NULL) {
	    ctxt->nameMax /= 2;
	    goto mem_error;
        }
	ctxt->nameTab = tmp;
    }
    ctxt->nameTab[ctxt->nameNr] = value;
    ctxt->name = value;
    return (ctxt->nameNr++);
mem_error:
    xmlErrMemory(ctxt, NULL);
    return (-1);
}
/**
 * namePop:
 * @ctxt: an XML parser context
 *
 * Pops the top element name from the name stack
 *
 * Returns the name just removed
 */
extern const xmlChar *
namePop(xmlParserCtxtPtr ctxt)
{
    const xmlChar *ret;

    if (ctxt->nameNr <= 0)
        return (0);
    ctxt->nameNr--;
    if (ctxt->nameNr > 0)
        ctxt->name = ctxt->nameTab[ctxt->nameNr - 1];
    else
        ctxt->name = NULL;
    ret = ctxt->nameTab[ctxt->nameNr];
    ctxt->nameTab[ctxt->nameNr] = 0;
    return (ret);
}

static int spacePush(xmlParserCtxtPtr ctxt, int val) {
    if (ctxt->spaceNr >= ctxt->spaceMax) {
	ctxt->spaceMax *= 2;
        ctxt->spaceTab = (int *) xmlRealloc(ctxt->spaceTab,
	             ctxt->spaceMax * sizeof(ctxt->spaceTab[0]));
        if (ctxt->spaceTab == NULL) {
	    xmlErrMemory(ctxt, NULL);
	    return(0);
	}
    }
    ctxt->spaceTab[ctxt->spaceNr] = val;
    ctxt->space = &ctxt->spaceTab[ctxt->spaceNr];
    return(ctxt->spaceNr++);
}

static int spacePop(xmlParserCtxtPtr ctxt) {
    int ret;
    if (ctxt->spaceNr <= 0) return(0);
    ctxt->spaceNr--;
    if (ctxt->spaceNr > 0)
	ctxt->space = &ctxt->spaceTab[ctxt->spaceNr - 1];
    else
        ctxt->space = NULL;
    ret = ctxt->spaceTab[ctxt->spaceNr];
    ctxt->spaceTab[ctxt->spaceNr] = -1;
    return(ret);
}

/*
 * Macros for accessing the content. Those should be used only by the parser,
 * and not exported.
 *
 * Dirty macros, i.e. one often need to make assumption on the context to
 * use them
 *
 *   CUR_PTR return the current pointer to the xmlChar to be parsed.
 *           To be used with extreme caution since operations consuming
 *           characters may move the input buffer to a different location !
 *   CUR     returns the current xmlChar value, i.e. a 8 bit value if compiled
 *           This should be used internally by the parser
 *           only to compare to ASCII values otherwise it would break when
 *           running with UTF-8 encoding.
 *   RAW     same as CUR but in the input buffer, bypass any token
 *           extraction that may have been done
 *   NXT(n)  returns the n'th next xmlChar. Same as CUR is should be used only
 *           to compare on ASCII based substring.
 *   SKIP(n) Skip n xmlChar, and must also be used only to skip ASCII defined
 *           strings without newlines within the parser.
 *   NEXT1(l) Skip 1 xmlChar, and must also be used only to skip 1 non-newline ASCII 
 *           defined char within the parser.
 * Clean macros, not dependent of an ASCII context, expect UTF-8 encoding
 *
 *   NEXT    Skip to the next character, this does the proper decoding
 *           in UTF-8 mode. It also pop-up unfinished entities on the fly.
 *   NEXTL(l) Skip the current unicode character of l xmlChars long.
 *   CUR_CHAR(l) returns the current unicode character (int), set l
 *           to the number of xmlChars used for the encoding [0-5].
 *   CUR_SCHAR  same but operate on a string instead of the context
 *   COPY_BUF  copy the current unicode char to the target buffer, increment
 *            the index
 *   GROW, SHRINK  handling of input buffers
 */

#define RAW (*ctxt->input->cur)
#define CUR (*ctxt->input->cur)
#define NXT(val) ctxt->input->cur[(val)]
#define CUR_PTR ctxt->input->cur

#define CMP4( s, c1, c2, c3, c4 ) \
  ( ((unsigned char *) s)[ 0 ] == c1 && ((unsigned char *) s)[ 1 ] == c2 && \
    ((unsigned char *) s)[ 2 ] == c3 && ((unsigned char *) s)[ 3 ] == c4 )
#define CMP5( s, c1, c2, c3, c4, c5 ) \
  ( CMP4( s, c1, c2, c3, c4 ) && ((unsigned char *) s)[ 4 ] == c5 )
#define CMP6( s, c1, c2, c3, c4, c5, c6 ) \
  ( CMP5( s, c1, c2, c3, c4, c5 ) && ((unsigned char *) s)[ 5 ] == c6 )
#define CMP7( s, c1, c2, c3, c4, c5, c6, c7 ) \
  ( CMP6( s, c1, c2, c3, c4, c5, c6 ) && ((unsigned char *) s)[ 6 ] == c7 )
#define CMP8( s, c1, c2, c3, c4, c5, c6, c7, c8 ) \
  ( CMP7( s, c1, c2, c3, c4, c5, c6, c7 ) && ((unsigned char *) s)[ 7 ] == c8 )
#define CMP9( s, c1, c2, c3, c4, c5, c6, c7, c8, c9 ) \
  ( CMP8( s, c1, c2, c3, c4, c5, c6, c7, c8 ) && \
    ((unsigned char *) s)[ 8 ] == c9 )
#define CMP10( s, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10 ) \
  ( CMP9( s, c1, c2, c3, c4, c5, c6, c7, c8, c9 ) && \
    ((unsigned char *) s)[ 9 ] == c10 )

#define SKIP(val) do {							\
    ctxt->nbChars += (val),ctxt->input->cur += (val),ctxt->input->col+=(val);			\
    if (*ctxt->input->cur == '%') xmlParserHandlePEReference(ctxt);	\
    if ((*ctxt->input->cur == 0) &&					\
        (xmlParserInputGrow(ctxt->input, INPUT_CHUNK) <= 0))		\
	    xmlPopInput(ctxt);						\
  } while (0)

#define SHRINK if ((ctxt->progressive == 0) &&				\
		   (ctxt->input->cur - ctxt->input->base > 2 * INPUT_CHUNK) && \
		   (ctxt->input->end - ctxt->input->cur < 2 * INPUT_CHUNK)) \
	xmlSHRINK (ctxt);

static void xmlSHRINK (xmlParserCtxtPtr ctxt) {
    xmlParserInputShrink(ctxt->input);
    if ((*ctxt->input->cur == 0) &&
        (xmlParserInputGrow(ctxt->input, INPUT_CHUNK) <= 0))
	    xmlPopInput(ctxt);
  }

#define GROW if ((ctxt->progressive == 0) &&				\
		 (ctxt->input->end - ctxt->input->cur < INPUT_CHUNK))	\
	xmlGROW (ctxt);

static void xmlGROW (xmlParserCtxtPtr ctxt) {
    xmlParserInputGrow(ctxt->input, INPUT_CHUNK);
    if ((*ctxt->input->cur == 0) &&
        (xmlParserInputGrow(ctxt->input, INPUT_CHUNK) <= 0))
	    xmlPopInput(ctxt);
}

#define SKIP_BLANKS xmlSkipBlankChars(ctxt)

#define NEXT xmlNextChar(ctxt)

#define NEXT1 {								\
	ctxt->input->col++;						\
	ctxt->input->cur++;						\
	ctxt->nbChars++;						\
	if (*ctxt->input->cur == 0)					\
	    xmlParserInputGrow(ctxt->input, INPUT_CHUNK);		\
    }

#define NEXTL(l) do {							\
    if (*(ctxt->input->cur) == '\n') {					\
	ctxt->input->line++; ctxt->input->col = 1;			\
    } else ctxt->input->col++;						\
    ctxt->input->cur += l;				\
    if (*ctxt->input->cur == '%') xmlParserHandlePEReference(ctxt);	\
  } while (0)

#define CUR_CHAR(l) xmlCurrentChar(ctxt, &l)
#define CUR_SCHAR(s, l) xmlStringCurrentChar(ctxt, s, &l)

#define COPY_BUF(l,b,i,v)						\
    if (l == 1) b[i++] = (xmlChar) v;					\
    else i += xmlCopyCharMultiByte(&b[i],v)

/**
 * xmlSkipBlankChars:
 * @ctxt:  the XML parser context
 *
 * skip all blanks character found at that point in the input streams.
 * It pops up finished entities in the process if allowable at that point.
 *
 * Returns the number of space chars skipped
 */

int
xmlSkipBlankChars(xmlParserCtxtPtr ctxt) {
    int res = 0;

    /*
     * It's Okay to use CUR/NEXT here since all the blanks are on
     * the ASCII range.
     */
    if ((ctxt->inputNr == 1) && (ctxt->instate != XML_PARSER_DTD)) {
	const xmlChar *cur;
	/*
	 * if we are in the document content, go really fast
	 */
	cur = ctxt->input->cur;
	while (IS_BLANK_CH(*cur)) {
	    if (*cur == '\n') {
		ctxt->input->line++; ctxt->input->col = 1;
	    }
	    cur++;
	    res++;
	    if (*cur == 0) {
		ctxt->input->cur = cur;
		xmlParserInputGrow(ctxt->input, INPUT_CHUNK);
		cur = ctxt->input->cur;
	    }
	}
	ctxt->input->cur = cur;
    } else {
	int cur;
	do {
	    cur = CUR;
	    while (IS_BLANK(cur)) { /* CHECKED tstblanks.xml */
		NEXT;
		cur = CUR;
		res++;
	    }
	    while ((cur == 0) && (ctxt->inputNr > 1) &&
		   (ctxt->instate != XML_PARSER_COMMENT)) {
		xmlPopInput(ctxt);
		cur = CUR;
	    }
	    /*
	     * Need to handle support of entities branching here
	     */
	    if (*ctxt->input->cur == '%') xmlParserHandlePEReference(ctxt);
	} while (IS_BLANK(cur)); /* CHECKED tstblanks.xml */
    }
    return(res);
}

/************************************************************************
 *									*
 *		Commodity functions to handle entities			*
 *									*
 ************************************************************************/

/**
 * xmlPopInput:
 * @ctxt:  an XML parser context
 *
 * xmlPopInput: the current input pointed by ctxt->input came to an end
 *          pop it and return the next char.
 *
 * Returns the current xmlChar in the parser context
 */
xmlChar
xmlPopInput(xmlParserCtxtPtr ctxt) {
    if (ctxt->inputNr == 1) return(0); /* End of main Input */
    if (xmlParserDebugEntities)
	xmlGenericError(xmlGenericErrorContext,
		"Popping input %d\n", ctxt->inputNr);
    xmlFreeInputStream(inputPop(ctxt));
    if ((*ctxt->input->cur == 0) &&
        (xmlParserInputGrow(ctxt->input, INPUT_CHUNK) <= 0))
	    return(xmlPopInput(ctxt));
    return(CUR);
}

/**
 * xmlPushInput:
 * @ctxt:  an XML parser context
 * @input:  an XML parser input fragment (entity, XML fragment ...).
 *
 * xmlPushInput: switch to a new input stream which is stacked on top
 *               of the previous one(s).
 */
void
xmlPushInput(xmlParserCtxtPtr ctxt, xmlParserInputPtr input) {
    if (input == NULL) return;

    if (xmlParserDebugEntities) {
	if ((ctxt->input != NULL) && (ctxt->input->filename))
	    xmlGenericError(xmlGenericErrorContext,
		    "%s(%d): ", ctxt->input->filename,
		    ctxt->input->line);
	xmlGenericError(xmlGenericErrorContext,
		"Pushing input %d : %.30s\n", ctxt->inputNr+1, input->cur);
    }
    inputPush(ctxt, input);
    GROW;
}

/**
 * xmlParseCharRef:
 * @ctxt:  an XML parser context
 *
 * parse Reference declarations
 *
 * [66] CharRef ::= '&#' [0-9]+ ';' |
 *                  '&#x' [0-9a-fA-F]+ ';'
 *
 * [ WFC: Legal Character ]
 * Characters referred to using character references must match the
 * production for Char. 
 *
 * Returns the value parsed (as an int), 0 in case of error
 */
int
xmlParseCharRef(xmlParserCtxtPtr ctxt) {
    unsigned int val = 0;
    int count = 0;

    /*
     * Using RAW/CUR/NEXT is okay since we are working on ASCII range here
     */
    if ((RAW == '&') && (NXT(1) == '#') &&
        (NXT(2) == 'x')) {
	SKIP(3);
	GROW;
	while (RAW != ';') { /* loop blocked by count */
	    if (count++ > 20) {
		count = 0;
		GROW;
	    }
	    if ((RAW >= '0') && (RAW <= '9')) 
	        val = val * 16 + (CUR - '0');
	    else if ((RAW >= 'a') && (RAW <= 'f') && (count < 20))
	        val = val * 16 + (CUR - 'a') + 10;
	    else if ((RAW >= 'A') && (RAW <= 'F') && (count < 20))
	        val = val * 16 + (CUR - 'A') + 10;
	    else {
		xmlFatalErr(ctxt, XML_ERR_INVALID_HEX_CHARREF, NULL);
		val = 0;
		break;
	    }
	    NEXT;
	    count++;
	}
	if (RAW == ';') {
	    /* on purpose to avoid reentrancy problems with NEXT and SKIP */
	    ctxt->input->col++;
	    ctxt->nbChars ++;
	    ctxt->input->cur++;
	}
    } else if  ((RAW == '&') && (NXT(1) == '#')) {
	SKIP(2);
	GROW;
	while (RAW != ';') { /* loop blocked by count */
	    if (count++ > 20) {
		count = 0;
		GROW;
	    }
	    if ((RAW >= '0') && (RAW <= '9')) 
	        val = val * 10 + (CUR - '0');
	    else {
		xmlFatalErr(ctxt, XML_ERR_INVALID_DEC_CHARREF, NULL);
		val = 0;
		break;
	    }
	    NEXT;
	    count++;
	}
	if (RAW == ';') {
	    /* on purpose to avoid reentrancy problems with NEXT and SKIP */
	    ctxt->input->col++;
	    ctxt->nbChars ++;
	    ctxt->input->cur++;
	}
    } else {
        xmlFatalErr(ctxt, XML_ERR_INVALID_CHARREF, NULL);
    }

    /*
     * [ WFC: Legal Character ]
     * Characters referred to using character references must match the
     * production for Char. 
     */
    if (IS_CHAR(val)) {
        return(val);
    } else {
        xmlFatalErrMsgInt(ctxt, XML_ERR_INVALID_CHAR,
                          "xmlParseCharRef: invalid xmlChar value %d\n",
	                  val);
    }
    return(0);
}

/**
 * xmlParseStringCharRef:
 * @ctxt:  an XML parser context
 * @str:  a pointer to an index in the string
 *
 * parse Reference declarations, variant parsing from a string rather
 * than an an input flow.
 *
 * [66] CharRef ::= '&#' [0-9]+ ';' |
 *                  '&#x' [0-9a-fA-F]+ ';'
 *
 * [ WFC: Legal Character ]
 * Characters referred to using character references must match the
 * production for Char. 
 *
 * Returns the value parsed (as an int), 0 in case of error, str will be
 *         updated to the current value of the index
 */
static int
xmlParseStringCharRef(xmlParserCtxtPtr ctxt, const xmlChar **str) {
    const xmlChar *ptr;
    xmlChar cur;
    int val = 0;

    if ((str == NULL) || (*str == NULL)) return(0);
    ptr = *str;
    cur = *ptr;
    if ((cur == '&') && (ptr[1] == '#') && (ptr[2] == 'x')) {
	ptr += 3;
	cur = *ptr;
	while (cur != ';') { /* Non input consuming loop */
	    if ((cur >= '0') && (cur <= '9')) 
	        val = val * 16 + (cur - '0');
	    else if ((cur >= 'a') && (cur <= 'f'))
	        val = val * 16 + (cur - 'a') + 10;
	    else if ((cur >= 'A') && (cur <= 'F'))
	        val = val * 16 + (cur - 'A') + 10;
	    else {
		xmlFatalErr(ctxt, XML_ERR_INVALID_HEX_CHARREF, NULL);
		val = 0;
		break;
	    }
	    ptr++;
	    cur = *ptr;
	}
	if (cur == ';')
	    ptr++;
    } else if  ((cur == '&') && (ptr[1] == '#')){
	ptr += 2;
	cur = *ptr;
	while (cur != ';') { /* Non input consuming loops */
	    if ((cur >= '0') && (cur <= '9')) 
	        val = val * 10 + (cur - '0');
	    else {
		xmlFatalErr(ctxt, XML_ERR_INVALID_DEC_CHARREF, NULL);
		val = 0;
		break;
	    }
	    ptr++;
	    cur = *ptr;
	}
	if (cur == ';')
	    ptr++;
    } else {
	xmlFatalErr(ctxt, XML_ERR_INVALID_CHARREF, NULL);
	return(0);
    }
    *str = ptr;

    /*
     * [ WFC: Legal Character ]
     * Characters referred to using character references must match the
     * production for Char. 
     */
    if (IS_CHAR(val)) {
        return(val);
    } else {
        xmlFatalErrMsgInt(ctxt, XML_ERR_INVALID_CHAR,
			  "xmlParseStringCharRef: invalid xmlChar value %d\n",
			  val);
    }
    return(0);
}

/**
 * xmlNewBlanksWrapperInputStream:
 * @ctxt:  an XML parser context
 * @entity:  an Entity pointer
 *
 * Create a new input stream for wrapping
 * blanks around a PEReference
 *
 * Returns the new input stream or NULL
 */
 
static void deallocblankswrapper (xmlChar *str) {xmlFree(str);}
 
static xmlParserInputPtr
xmlNewBlanksWrapperInputStream(xmlParserCtxtPtr ctxt, xmlEntityPtr entity) {
    xmlParserInputPtr input;
    xmlChar *buffer;
    size_t length;
    if (entity == NULL) {
	xmlFatalErr(ctxt, XML_ERR_INTERNAL_ERROR,
	            "xmlNewBlanksWrapperInputStream entity\n");
	return(NULL);
    }
    if (xmlParserDebugEntities)
	xmlGenericError(xmlGenericErrorContext,
		"new blanks wrapper for entity: %s\n", entity->name);
    input = xmlNewInputStream(ctxt);
    if (input == NULL) {
	return(NULL);
    }
    length = xmlStrlen(entity->name) + 5;
    buffer = xmlMallocAtomic(length);
    if (buffer == NULL) {
	xmlErrMemory(ctxt, NULL);
    	return(NULL);
    }
    buffer [0] = ' ';
    buffer [1] = '%';
    buffer [length-3] = ';';
    buffer [length-2] = ' ';
    buffer [length-1] = 0;
    memcpy(buffer + 2, entity->name, length - 5);
    input->free = deallocblankswrapper;
    input->base = buffer;
    input->cur = buffer;
    input->length = length;
    input->end = &buffer[length];
    return(input);
}

/**
 * xmlParserHandlePEReference:
 * @ctxt:  the parser context
 * 
 * [69] PEReference ::= '%' Name ';'
 *
 * [ WFC: No Recursion ]
 * A parsed entity must not contain a recursive
 * reference to itself, either directly or indirectly. 
 *
 * [ WFC: Entity Declared ]
 * In a document without any DTD, a document with only an internal DTD
 * subset which contains no parameter entity references, or a document
 * with "standalone='yes'", ...  ... The declaration of a parameter
 * entity must precede any reference to it...
 *
 * [ VC: Entity Declared ]
 * In a document with an external subset or external parameter entities
 * with "standalone='no'", ...  ... The declaration of a parameter entity
 * must precede any reference to it...
 *
 * [ WFC: In DTD ]
 * Parameter-entity references may only appear in the DTD.
 * NOTE: misleading but this is handled.
 *
 * A PEReference may have been detected in the current input stream
 * the handling is done accordingly to 
 *      http://www.w3.org/TR/REC-xml#entproc
 * i.e. 
 *   - Included in literal in entity values
 *   - Included as Parameter Entity reference within DTDs
 */
void
xmlParserHandlePEReference(xmlParserCtxtPtr ctxt) {
    const xmlChar *name;
    xmlEntityPtr entity = NULL;
    xmlParserInputPtr input;

    if (RAW != '%') return;
    switch(ctxt->instate) {
	case XML_PARSER_CDATA_SECTION:
	    return;
        case XML_PARSER_COMMENT:
	    return;
	case XML_PARSER_START_TAG:
	    return;
	case XML_PARSER_END_TAG:
	    return;
        case XML_PARSER_EOF:
	    xmlFatalErr(ctxt, XML_ERR_PEREF_AT_EOF, NULL);
	    return;
        case XML_PARSER_PROLOG:
	case XML_PARSER_START:
	case XML_PARSER_MISC:
	    xmlFatalErr(ctxt, XML_ERR_PEREF_IN_PROLOG, NULL);
	    return;
	case XML_PARSER_ENTITY_DECL:
        case XML_PARSER_CONTENT:
        case XML_PARSER_ATTRIBUTE_VALUE:
        case XML_PARSER_PI:
	case XML_PARSER_SYSTEM_LITERAL:
	case XML_PARSER_PUBLIC_LITERAL:
	    /* we just ignore it there */
	    return;
        case XML_PARSER_EPILOG:
	    xmlFatalErr(ctxt, XML_ERR_PEREF_IN_EPILOG, NULL);
	    return;
	case XML_PARSER_ENTITY_VALUE:
	    /*
	     * NOTE: in the case of entity values, we don't do the
	     *       substitution here since we need the literal
	     *       entity value to be able to save the internal
	     *       subset of the document.
	     *       This will be handled by xmlStringDecodeEntities
	     */
	    return;
        case XML_PARSER_DTD:
	    /*
	     * [WFC: Well-Formedness Constraint: PEs in Internal Subset]
	     * In the internal DTD subset, parameter-entity references
	     * can occur only where markup declarations can occur, not
	     * within markup declarations.
	     * In that case this is handled in xmlParseMarkupDecl
	     */
	    if ((ctxt->external == 0) && (ctxt->inputNr == 1))
		return;
	    if (IS_BLANK_CH(NXT(1)) || NXT(1) == 0)
		return;
            break;
        case XML_PARSER_IGNORE:
            return;
    }

    NEXT;
    name = xmlParseName(ctxt);
    if (xmlParserDebugEntities)
	xmlGenericError(xmlGenericErrorContext,
		"PEReference: %s\n", name);
    if (name == NULL) {
	xmlFatalErr(ctxt, XML_ERR_PEREF_NO_NAME, NULL);
    } else {
	if (RAW == ';') {
	    NEXT;
	    if ((ctxt->sax != NULL) && (ctxt->sax->getParameterEntity != NULL))
		entity = ctxt->sax->getParameterEntity(ctxt->userData, name);
	    if (entity == NULL) {
	        
		/*
		 * [ WFC: Entity Declared ]
		 * In a document without any DTD, a document with only an
		 * internal DTD subset which contains no parameter entity
		 * references, or a document with "standalone='yes'", ...
		 * ... The declaration of a parameter entity must precede
		 * any reference to it...
		 */
		if ((ctxt->standalone == 1) ||
		    ((ctxt->hasExternalSubset == 0) &&
		     (ctxt->hasPErefs == 0))) {
		    xmlFatalErrMsgStr(ctxt, XML_ERR_UNDECLARED_ENTITY,
			 "PEReference: %%%s; not found\n", name);
	        } else {
		    /*
		     * [ VC: Entity Declared ]
		     * In a document with an external subset or external
		     * parameter entities with "standalone='no'", ...
		     * ... The declaration of a parameter entity must precede
		     * any reference to it...
		     */
		    if ((ctxt->validate) && (ctxt->vctxt.error != NULL)) {
		        xmlValidityError(ctxt, XML_WAR_UNDECLARED_ENTITY,
			                 "PEReference: %%%s; not found\n",
				         name);
		    } else 
		        xmlWarningMsg(ctxt, XML_WAR_UNDECLARED_ENTITY,
			              "PEReference: %%%s; not found\n",
				      name, NULL);
		    ctxt->valid = 0;
		}
	    } else if (ctxt->input->free != deallocblankswrapper) {
		    input = xmlNewBlanksWrapperInputStream(ctxt, entity);
		    xmlPushInput(ctxt, input);
	    } else {
	        if ((entity->etype == XML_INTERNAL_PARAMETER_ENTITY) ||
		    (entity->etype == XML_EXTERNAL_PARAMETER_ENTITY)) {
		    xmlChar start[4];
		    xmlCharEncoding enc;

		    /*
		     * handle the extra spaces added before and after
		     * c.f. http://www.w3.org/TR/REC-xml#as-PE
		     * this is done independently.
		     */
		    input = xmlNewEntityInputStream(ctxt, entity);
		    xmlPushInput(ctxt, input);

		    /* 
		     * Get the 4 first bytes and decode the charset
		     * if enc != XML_CHAR_ENCODING_NONE
		     * plug some encoding conversion routines.
		     */
		    GROW
	            if (entity->length >= 4) {
			start[0] = RAW;
			start[1] = NXT(1);
			start[2] = NXT(2);
			start[3] = NXT(3);
			enc = xmlDetectCharEncoding(start, 4);
			if (enc != XML_CHAR_ENCODING_NONE) {
			    xmlSwitchEncoding(ctxt, enc);
			}
		    }

		    if ((entity->etype == XML_EXTERNAL_PARAMETER_ENTITY) &&
			(CMP5(CUR_PTR, '<', '?', 'x', 'm', 'l' )) &&
			(IS_BLANK_CH(NXT(5)))) {
			xmlParseTextDecl(ctxt);
		    }
		} else {
		    xmlFatalErrMsgStr(ctxt, XML_ERR_ENTITY_IS_PARAMETER,
			     "PEReference: %s is not a parameter entity\n",
				      name);
		}
	    }
	} else {
	    xmlFatalErr(ctxt, XML_ERR_PEREF_SEMICOL_MISSING, NULL);
	}
    }
}

/*
 * Macro used to grow the current buffer.
 */
#define growBuffer(buffer) {						\
    buffer##_size *= 2;							\
    buffer = (xmlChar *)						\
    		xmlRealloc(buffer, buffer##_size * sizeof(xmlChar));	\
    if (buffer == NULL) goto mem_error;					\
}

/**
 * xmlStringLenDecodeEntities:
 * @ctxt:  the parser context
 * @str:  the input string
 * @len: the string length
 * @what:  combination of XML_SUBSTITUTE_REF and XML_SUBSTITUTE_PEREF
 * @end:  an end marker xmlChar, 0 if none
 * @end2:  an end marker xmlChar, 0 if none
 * @end3:  an end marker xmlChar, 0 if none
 * 
 * Takes a entity string content and process to do the adequate substitutions.
 *
 * [67] Reference ::= EntityRef | CharRef
 *
 * [69] PEReference ::= '%' Name ';'
 *
 * Returns A newly allocated string with the substitution done. The caller
 *      must deallocate it !
 */
xmlChar *
xmlStringLenDecodeEntities(xmlParserCtxtPtr ctxt, const xmlChar *str, int len,
		      int what, xmlChar end, xmlChar  end2, xmlChar end3) {
    xmlChar *buffer = NULL;
    int buffer_size = 0;

    xmlChar *current = NULL;
    const xmlChar *last;
    xmlEntityPtr ent;
    int c,l;
    int nbchars = 0;

    if ((str == NULL) || (len < 0))
	return(NULL);
    last = str + len;

    if (ctxt->depth > 40) {
	xmlFatalErr(ctxt, XML_ERR_ENTITY_LOOP, NULL);
	return(NULL);
    }

    /*
     * allocate a translation buffer.
     */
    buffer_size = XML_PARSER_BIG_BUFFER_SIZE;
    buffer = (xmlChar *) xmlMallocAtomic(buffer_size * sizeof(xmlChar));
    if (buffer == NULL) goto mem_error;

    /*
     * OK loop until we reach one of the ending char or a size limit.
     * we are operating on already parsed values.
     */
    if (str < last)
	c = CUR_SCHAR(str, l);
    else
        c = 0;
    while ((c != 0) && (c != end) && /* non input consuming loop */
	   (c != end2) && (c != end3)) {

	if (c == 0) break;
        if ((c == '&') && (str[1] == '#')) {
	    int val = xmlParseStringCharRef(ctxt, &str);
	    if (val != 0) {
		COPY_BUF(0,buffer,nbchars,val);
	    }
	} else if ((c == '&') && (what & XML_SUBSTITUTE_REF)) {
	    if (xmlParserDebugEntities)
		xmlGenericError(xmlGenericErrorContext,
			"String decoding Entity Reference: %.30s\n",
			str);
	    ent = xmlParseStringEntityRef(ctxt, &str);
	    if ((ent != NULL) &&
		(ent->etype == XML_INTERNAL_PREDEFINED_ENTITY)) {
		if (ent->content != NULL) {
		    COPY_BUF(0,buffer,nbchars,ent->content[0]);
		} else {
		    xmlFatalErrMsg(ctxt, XML_ERR_INTERNAL_ERROR,
			    "predefined entity has no content\n");
		}
	    } else if ((ent != NULL) && (ent->content != NULL)) {
		xmlChar *rep;

		ctxt->depth++;
		rep = xmlStringDecodeEntities(ctxt, ent->content, what,
			                      0, 0, 0);
		ctxt->depth--;
		if (rep != NULL) {
		    current = rep;
		    while (*current != 0) { /* non input consuming loop */
			buffer[nbchars++] = *current++;
			if (nbchars >
		            buffer_size - XML_PARSER_BUFFER_SIZE) {
			    growBuffer(buffer);
			}
		    }
		    xmlFree(rep);
		}
	    } else if (ent != NULL) {
		int i = xmlStrlen(ent->name);
		const xmlChar *cur = ent->name;

		buffer[nbchars++] = '&';
		if (nbchars > buffer_size - i - XML_PARSER_BUFFER_SIZE) {
		    growBuffer(buffer);
		}
		for (;i > 0;i--)
		    buffer[nbchars++] = *cur++;
		buffer[nbchars++] = ';';
	    }
	} else if (c == '%' && (what & XML_SUBSTITUTE_PEREF)) {
	    if (xmlParserDebugEntities)
		xmlGenericError(xmlGenericErrorContext,
			"String decoding PE Reference: %.30s\n", str);
	    ent = xmlParseStringPEReference(ctxt, &str);
	    if (ent != NULL) {
		xmlChar *rep;

		ctxt->depth++;
		rep = xmlStringDecodeEntities(ctxt, ent->content, what,
			                      0, 0, 0);
		ctxt->depth--;
		if (rep != NULL) {
		    current = rep;
		    while (*current != 0) { /* non input consuming loop */
			buffer[nbchars++] = *current++;
			if (nbchars >
		            buffer_size - XML_PARSER_BUFFER_SIZE) {
			    growBuffer(buffer);
			}
		    }
		    xmlFree(rep);
		}
	    }
	} else {
	    COPY_BUF(l,buffer,nbchars,c);
	    str += l;
	    if (nbchars > buffer_size - XML_PARSER_BUFFER_SIZE) {
	      growBuffer(buffer);
	    }
	}
	if (str < last)
	    c = CUR_SCHAR(str, l);
	else
	    c = 0;
    }
    buffer[nbchars++] = 0;
    return(buffer);

mem_error:
    xmlErrMemory(ctxt, NULL);
    return(NULL);
}

/**
 * xmlStringDecodeEntities:
 * @ctxt:  the parser context
 * @str:  the input string
 * @what:  combination of XML_SUBSTITUTE_REF and XML_SUBSTITUTE_PEREF
 * @end:  an end marker xmlChar, 0 if none
 * @end2:  an end marker xmlChar, 0 if none
 * @end3:  an end marker xmlChar, 0 if none
 * 
 * Takes a entity string content and process to do the adequate substitutions.
 *
 * [67] Reference ::= EntityRef | CharRef
 *
 * [69] PEReference ::= '%' Name ';'
 *
 * Returns A newly allocated string with the substitution done. The caller
 *      must deallocate it !
 */
xmlChar *
xmlStringDecodeEntities(xmlParserCtxtPtr ctxt, const xmlChar *str, int what,
		        xmlChar end, xmlChar  end2, xmlChar end3) {
    return(xmlStringLenDecodeEntities(ctxt, str, xmlStrlen(str), what,
           end, end2, end3));
}

/************************************************************************
 *									*
 *		Commodity functions to handle xmlChars			*
 *									*
 ************************************************************************/

/**
 * xmlStrndup:
 * @cur:  the input xmlChar *
 * @len:  the len of @cur
 *
 * a strndup for array of xmlChar's
 *
 * Returns a new xmlChar * or NULL
 */
xmlChar *
xmlStrndup(const xmlChar *cur, int len) {
    xmlChar *ret;
    
    if ((cur == NULL) || (len < 0)) return(NULL);
    ret = (xmlChar *) xmlMallocAtomic((len + 1) * sizeof(xmlChar));
    if (ret == NULL) {
        xmlErrMemory(NULL, NULL);
        return(NULL);
    }
    memcpy(ret, cur, len * sizeof(xmlChar));
    ret[len] = 0;
    return(ret);
}

/**
 * xmlStrdup:
 * @cur:  the input xmlChar *
 *
 * a strdup for array of xmlChar's. Since they are supposed to be
 * encoded in UTF-8 or an encoding with 8bit based chars, we assume
 * a termination mark of '0'.
 *
 * Returns a new xmlChar * or NULL
 */
xmlChar *
xmlStrdup(const xmlChar *cur) {
    const xmlChar *p = cur;

    if (cur == NULL) return(NULL);
    while (*p != 0) p++; /* non input consuming */
    return(xmlStrndup(cur, p - cur));
}

/**
 * xmlCharStrndup:
 * @cur:  the input char *
 * @len:  the len of @cur
 *
 * a strndup for char's to xmlChar's
 *
 * Returns a new xmlChar * or NULL
 */

xmlChar *
xmlCharStrndup(const char *cur, int len) {
    int i;
    xmlChar *ret;
    
    if ((cur == NULL) || (len < 0)) return(NULL);
    ret = (xmlChar *) xmlMallocAtomic((len + 1) * sizeof(xmlChar));
    if (ret == NULL) {
        xmlErrMemory(NULL, NULL);
        return(NULL);
    }
    for (i = 0;i < len;i++)
        ret[i] = (xmlChar) cur[i];
    ret[len] = 0;
    return(ret);
}

/**
 * xmlCharStrdup:
 * @cur:  the input char *
 *
 * a strdup for char's to xmlChar's
 *
 * Returns a new xmlChar * or NULL
 */

xmlChar *
xmlCharStrdup(const char *cur) {
    const char *p = cur;

    if (cur == NULL) return(NULL);
    while (*p != '\0') p++; /* non input consuming */
    return(xmlCharStrndup(cur, p - cur));
}

/**
 * xmlStrcmp:
 * @str1:  the first xmlChar *
 * @str2:  the second xmlChar *
 *
 * a strcmp for xmlChar's
 *
 * Returns the integer result of the comparison
 */

int
xmlStrcmp(const xmlChar *str1, const xmlChar *str2) {
    register int tmp;

    if (str1 == str2) return(0);
    if (str1 == NULL) return(-1);
    if (str2 == NULL) return(1);
    do {
        tmp = *str1++ - *str2;
	if (tmp != 0) return(tmp);
    } while (*str2++ != 0);
    return 0;
}

/**
 * xmlStrEqual:
 * @str1:  the first xmlChar *
 * @str2:  the second xmlChar *
 *
 * Check if both string are equal of have same content
 * Should be a bit more readable and faster than xmlStrEqual()
 *
 * Returns 1 if they are equal, 0 if they are different
 */

int
xmlStrEqual(const xmlChar *str1, const xmlChar *str2) {
    if (str1 == str2) return(1);
    if (str1 == NULL) return(0);
    if (str2 == NULL) return(0);
    do {
	if (*str1++ != *str2) return(0);
    } while (*str2++);
    return(1);
}

/**
 * xmlStrQEqual:
 * @pref:  the prefix of the QName
 * @name:  the localname of the QName
 * @str:  the second xmlChar *
 *
 * Check if a QName is Equal to a given string 
 *
 * Returns 1 if they are equal, 0 if they are different
 */

int
xmlStrQEqual(const xmlChar *pref, const xmlChar *name, const xmlChar *str) {
    if (pref == NULL) return(xmlStrEqual(name, str));
    if (name == NULL) return(0);
    if (str == NULL) return(0);

    do {
	if (*pref++ != *str) return(0);
    } while ((*str++) && (*pref));
    if (*str++ != ':') return(0);
    do {
	if (*name++ != *str) return(0);
    } while (*str++);
    return(1);
}

/**
 * xmlStrncmp:
 * @str1:  the first xmlChar *
 * @str2:  the second xmlChar *
 * @len:  the max comparison length
 *
 * a strncmp for xmlChar's
 *
 * Returns the integer result of the comparison
 */

int
xmlStrncmp(const xmlChar *str1, const xmlChar *str2, int len) {
    register int tmp;

    if (len <= 0) return(0);
    if (str1 == str2) return(0);
    if (str1 == NULL) return(-1);
    if (str2 == NULL) return(1);
    do {
        tmp = *str1++ - *str2;
	if (tmp != 0 || --len == 0) return(tmp);
    } while (*str2++ != 0);
    return 0;
}

static const xmlChar casemap[256] = {
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
    0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
    0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
    0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F,
    0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,
    0x28,0x29,0x2A,0x2B,0x2C,0x2D,0x2E,0x2F,
    0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,
    0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,
    0x40,0x61,0x62,0x63,0x64,0x65,0x66,0x67,
    0x68,0x69,0x6A,0x6B,0x6C,0x6D,0x6E,0x6F,
    0x70,0x71,0x72,0x73,0x74,0x75,0x76,0x77,
    0x78,0x79,0x7A,0x7B,0x5C,0x5D,0x5E,0x5F,
    0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67,
    0x68,0x69,0x6A,0x6B,0x6C,0x6D,0x6E,0x6F,
    0x70,0x71,0x72,0x73,0x74,0x75,0x76,0x77,
    0x78,0x79,0x7A,0x7B,0x7C,0x7D,0x7E,0x7F,
    0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8A,0x8B,0x8C,0x8D,0x8E,0x8F,
    0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97,
    0x98,0x99,0x9A,0x9B,0x9C,0x9D,0x9E,0x9F,
    0xA0,0xA1,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,
    0xA8,0xA9,0xAA,0xAB,0xAC,0xAD,0xAE,0xAF,
    0xB0,0xB1,0xB2,0xB3,0xB4,0xB5,0xB6,0xB7,
    0xB8,0xB9,0xBA,0xBB,0xBC,0xBD,0xBE,0xBF,
    0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,
    0xC8,0xC9,0xCA,0xCB,0xCC,0xCD,0xCE,0xCF,
    0xD0,0xD1,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,
    0xD8,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE,0xDF,
    0xE0,0xE1,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,
    0xE8,0xE9,0xEA,0xEB,0xEC,0xED,0xEE,0xEF,
    0xF0,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,
    0xF8,0xF9,0xFA,0xFB,0xFC,0xFD,0xFE,0xFF
};

/**
 * xmlStrcasecmp:
 * @str1:  the first xmlChar *
 * @str2:  the second xmlChar *
 *
 * a strcasecmp for xmlChar's
 *
 * Returns the integer result of the comparison
 */

int
xmlStrcasecmp(const xmlChar *str1, const xmlChar *str2) {
    register int tmp;

    if (str1 == str2) return(0);
    if (str1 == NULL) return(-1);
    if (str2 == NULL) return(1);
    do {
	tmp = casemap[*str1++] - casemap[*str2];
	if (tmp != 0) return(tmp);
    } while (*str2++ != 0);
    return 0;
}

/**
 * xmlStrncasecmp:
 * @str1:  the first xmlChar *
 * @str2:  the second xmlChar *
 * @len:  the max comparison length
 *
 * a strncasecmp for xmlChar's
 *
 * Returns the integer result of the comparison
 */

int
xmlStrncasecmp(const xmlChar *str1, const xmlChar *str2, int len) {
    register int tmp;

    if (len <= 0) return(0);
    if (str1 == str2) return(0);
    if (str1 == NULL) return(-1);
    if (str2 == NULL) return(1);
    do {
	tmp = casemap[*str1++] - casemap[*str2];
	if (tmp != 0 || --len == 0) return(tmp);
    } while (*str2++ != 0);
    return 0;
}

/**
 * xmlStrchr:
 * @str:  the xmlChar * array
 * @val:  the xmlChar to search
 *
 * a strchr for xmlChar's
 *
 * Returns the xmlChar * for the first occurrence or NULL.
 */

const xmlChar *
xmlStrchr(const xmlChar *str, xmlChar val) {
    if (str == NULL) return(NULL);
    while (*str != 0) { /* non input consuming */
        if (*str == val) return((xmlChar *) str);
	str++;
    }
    return(NULL);
}

/**
 * xmlStrstr:
 * @str:  the xmlChar * array (haystack)
 * @val:  the xmlChar to search (needle)
 *
 * a strstr for xmlChar's
 *
 * Returns the xmlChar * for the first occurrence or NULL.
 */

const xmlChar *
xmlStrstr(const xmlChar *str, const xmlChar *val) {
    int n;
    
    if (str == NULL) return(NULL);
    if (val == NULL) return(NULL);
    n = xmlStrlen(val);

    if (n == 0) return(str);
    while (*str != 0) { /* non input consuming */
        if (*str == *val) {
	    if (!xmlStrncmp(str, val, n)) return((const xmlChar *) str);
	}
	str++;
    }
    return(NULL);
}

/**
 * xmlStrcasestr:
 * @str:  the xmlChar * array (haystack)
 * @val:  the xmlChar to search (needle)
 *
 * a case-ignoring strstr for xmlChar's
 *
 * Returns the xmlChar * for the first occurrence or NULL.
 */

const xmlChar *
xmlStrcasestr(const xmlChar *str, xmlChar *val) {
    int n;
    
    if (str == NULL) return(NULL);
    if (val == NULL) return(NULL);
    n = xmlStrlen(val);

    if (n == 0) return(str);
    while (*str != 0) { /* non input consuming */
	if (casemap[*str] == casemap[*val])
	    if (!xmlStrncasecmp(str, val, n)) return(str);
	str++;
    }
    return(NULL);
}

/**
 * xmlStrsub:
 * @str:  the xmlChar * array (haystack)
 * @start:  the index of the first char (zero based)
 * @len:  the length of the substring
 *
 * Extract a substring of a given string
 *
 * Returns the xmlChar * for the first occurrence or NULL.
 */

xmlChar *
xmlStrsub(const xmlChar *str, int start, int len) {
    int i;
    
    if (str == NULL) return(NULL);
    if (start < 0) return(NULL);
    if (len < 0) return(NULL);

    for (i = 0;i < start;i++) {
        if (*str == 0) return(NULL);
	str++;
    }
    if (*str == 0) return(NULL);
    return(xmlStrndup(str, len));
}

/**
 * xmlStrlen:
 * @str:  the xmlChar * array
 *
 * length of a xmlChar's string
 *
 * Returns the number of xmlChar contained in the ARRAY.
 */

int
xmlStrlen(const xmlChar *str) {
    int len = 0;

    if (str == NULL) return(0);
    while (*str != 0) { /* non input consuming */
	str++;
	len++;
    }
    return(len);
}

/**
 * xmlStrncat:
 * @cur:  the original xmlChar * array
 * @add:  the xmlChar * array added
 * @len:  the length of @add
 *
 * a strncat for array of xmlChar's, it will extend @cur with the len
 * first bytes of @add.
 *
 * Returns a new xmlChar *, the original @cur is reallocated if needed
 * and should not be freed
 */

xmlChar *
xmlStrncat(xmlChar *cur, const xmlChar *add, int len) {
    int size;
    xmlChar *ret;

    if ((add == NULL) || (len == 0))
        return(cur);
    if (cur == NULL)
        return(xmlStrndup(add, len));

    size = xmlStrlen(cur);
    ret = (xmlChar *) xmlRealloc(cur, (size + len + 1) * sizeof(xmlChar));
    if (ret == NULL) {
        xmlErrMemory(NULL, NULL);
        return(cur);
    }
    memcpy(&ret[size], add, len * sizeof(xmlChar));
    ret[size + len] = 0;
    return(ret);
}

/**
 * xmlStrcat:
 * @cur:  the original xmlChar * array
 * @add:  the xmlChar * array added
 *
 * a strcat for array of xmlChar's. Since they are supposed to be
 * encoded in UTF-8 or an encoding with 8bit based chars, we assume
 * a termination mark of '0'.
 *
 * Returns a new xmlChar * containing the concatenated string.
 */
xmlChar *
xmlStrcat(xmlChar *cur, const xmlChar *add) {
    const xmlChar *p = add;

    if (add == NULL) return(cur);
    if (cur == NULL) 
        return(xmlStrdup(add));

    while (*p != 0) p++; /* non input consuming */
    return(xmlStrncat(cur, add, p - add));
}

/**
 * xmlStrPrintf:
 * @buf:   the result buffer.
 * @len:   the result buffer length.
 * @msg:   the message with printf formatting.
 * @...:   extra parameters for the message.
 *
 * Formats @msg and places result into @buf.
 *
 * Returns the number of characters written to @buf or -1 if an error occurs.
 */
int 
xmlStrPrintf(xmlChar *buf, int len, const xmlChar *msg, ...) {
    va_list args;
    int ret;
    
    if((buf == NULL) || (msg == NULL)) {
	return(-1);
    }
    
    va_start(args, msg);
    ret = vsnprintf((char *) buf, len, (const char *) msg, args);
    va_end(args);
    buf[len - 1] = 0; /* be safe ! */
    
    return(ret);
}

/************************************************************************
 *									*
 *		Commodity functions, cleanup needed ?			*
 *									*
 ************************************************************************/

/**
 * areBlanks:
 * @ctxt:  an XML parser context
 * @str:  a xmlChar *
 * @len:  the size of @str
 *
 * Is this a sequence of blank chars that one can ignore ?
 *
 * Returns 1 if ignorable 0 otherwise.
 */

static int areBlanks(xmlParserCtxtPtr ctxt, const xmlChar *str, int len) {
    int i, ret;
    xmlNodePtr lastChild;

    /*
     * Don't spend time trying to differentiate them, the same callback is
     * used !
     */
    if (ctxt->sax->ignorableWhitespace == ctxt->sax->characters)
	return(0);

    /*
     * Check for xml:space value.
     */
    if (*(ctxt->space) == 1)
	return(0);

    /*
     * Check that the string is made of blanks
     */
    for (i = 0;i < len;i++)
        if (!(IS_BLANK_CH(str[i]))) return(0);

    /*
     * Look if the element is mixed content in the DTD if available
     */
    if (ctxt->node == NULL) return(0);
    if (ctxt->myDoc != NULL) {
	ret = xmlIsMixedElement(ctxt->myDoc, ctxt->node->name);
        if (ret == 0) return(1);
        if (ret == 1) return(0);
    }

    /*
     * Otherwise, heuristic :-\
     */
    if (RAW != '<') return(0);
    if ((ctxt->node->children == NULL) &&
	(RAW == '<') && (NXT(1) == '/')) return(0);

    lastChild = xmlGetLastChild(ctxt->node);
    if (lastChild == NULL) {
        if ((ctxt->node->type != XML_ELEMENT_NODE) &&
            (ctxt->node->content != NULL)) return(0);
    } else if (xmlNodeIsText(lastChild))
        return(0);
    else if ((ctxt->node->children != NULL) &&
             (xmlNodeIsText(ctxt->node->children)))
        return(0);
    return(1);
}

/************************************************************************
 *									*
 *		Extra stuff for namespace support			*
 *	Relates to http://www.w3.org/TR/WD-xml-names			*
 *									*
 ************************************************************************/

/**
 * xmlSplitQName:
 * @ctxt:  an XML parser context
 * @name:  an XML parser context
 * @prefix:  a xmlChar ** 
 *
 * parse an UTF8 encoded XML qualified name string
 *
 * [NS 5] QName ::= (Prefix ':')? LocalPart
 *
 * [NS 6] Prefix ::= NCName
 *
 * [NS 7] LocalPart ::= NCName
 *
 * Returns the local part, and prefix is updated
 *   to get the Prefix if any.
 */

xmlChar *
xmlSplitQName(xmlParserCtxtPtr ctxt, const xmlChar *name, xmlChar **prefix) {
    xmlChar buf[XML_MAX_NAMELEN + 5];
    xmlChar *buffer = NULL;
    int len = 0;
    int max = XML_MAX_NAMELEN;
    xmlChar *ret = NULL;
    const xmlChar *cur = name;
    int c;

    *prefix = NULL;

    if (cur == NULL) return(NULL);

#ifndef XML_XML_NAMESPACE
    /* xml: prefix is not really a namespace */
    if ((cur[0] == 'x') && (cur[1] == 'm') &&
        (cur[2] == 'l') && (cur[3] == ':'))
	return(xmlStrdup(name));
#endif

    /* nasty but well=formed */
    if (cur[0] == ':')
	return(xmlStrdup(name));

    c = *cur++;
    while ((c != 0) && (c != ':') && (len < max)) { /* tested bigname.xml */
	buf[len++] = c;
	c = *cur++;
    }
    if (len >= max) {
	/*
	 * Okay someone managed to make a huge name, so he's ready to pay
	 * for the processing speed.
	 */
	max = len * 2;
	
	buffer = (xmlChar *) xmlMallocAtomic(max * sizeof(xmlChar));
	if (buffer == NULL) {
	    xmlErrMemory(ctxt, NULL);
	    return(NULL);
	}
	memcpy(buffer, buf, len);
	while ((c != 0) && (c != ':')) { /* tested bigname.xml */
	    if (len + 10 > max) {
		max *= 2;
		buffer = (xmlChar *) xmlRealloc(buffer,
						max * sizeof(xmlChar));
		if (buffer == NULL) {
		    xmlErrMemory(ctxt, NULL);
		    return(NULL);
		}
	    }
	    buffer[len++] = c;
	    c = *cur++;
	}
	buffer[len] = 0;
    }
    
    /* nasty but well=formed
    if ((c == ':') && (*cur == 0)) {
	return(xmlStrdup(name));
    } */

    if (buffer == NULL)
	ret = xmlStrndup(buf, len);
    else {
	ret = buffer;
	buffer = NULL;
	max = XML_MAX_NAMELEN;
    }


    if (c == ':') {
	c = *cur;
        *prefix = ret;
	if (c == 0) {
	    return(xmlStrndup(BAD_CAST "", 0));
	}
	len = 0;

	/*
	 * Check that the first character is proper to start
	 * a new name
	 */
	if (!(((c >= 0x61) && (c <= 0x7A)) ||
	      ((c >= 0x41) && (c <= 0x5A)) ||
	      (c == '_') || (c == ':'))) {
	    int l;
	    int first = CUR_SCHAR(cur, l);

	    if (!IS_LETTER(first) && (first != '_')) {
		xmlFatalErrMsgStr(ctxt, XML_NS_ERR_QNAME,
			    "Name %s is not XML Namespace compliant\n",
				  name);
	    }
	}
	cur++;

	while ((c != 0) && (len < max)) { /* tested bigname2.xml */
	    buf[len++] = c;
	    c = *cur++;
	}
	if (len >= max) {
	    /*
	     * Okay someone managed to make a huge name, so he's ready to pay
	     * for the processing speed.
	     */
	    max = len * 2;
	    
	    buffer = (xmlChar *) xmlMallocAtomic(max * sizeof(xmlChar));
	    if (buffer == NULL) {
	        xmlErrMemory(ctxt, NULL);
		return(NULL);
	    }
	    memcpy(buffer, buf, len);
	    while (c != 0) { /* tested bigname2.xml */
		if (len + 10 > max) {
		    max *= 2;
		    buffer = (xmlChar *) xmlRealloc(buffer,
						    max * sizeof(xmlChar));
		    if (buffer == NULL) {
			xmlErrMemory(ctxt, NULL);
			return(NULL);
		    }
		}
		buffer[len++] = c;
		c = *cur++;
	    }
	    buffer[len] = 0;
	}
	
	if (buffer == NULL)
	    ret = xmlStrndup(buf, len);
	else {
	    ret = buffer;
	}
    }

    return(ret);
}

/************************************************************************
 *									*
 *			The parser itself				*
 *	Relates to http://www.w3.org/TR/REC-xml				*
 *									*
 ************************************************************************/

static const xmlChar * xmlParseNameComplex(xmlParserCtxtPtr ctxt);
static xmlChar * xmlParseAttValueInternal(xmlParserCtxtPtr ctxt,
                                          int *len, int *alloc, int normalize);

/**
 * xmlParseName:
 * @ctxt:  an XML parser context
 *
 * parse an XML name.
 *
 * [4] NameChar ::= Letter | Digit | '.' | '-' | '_' | ':' |
 *                  CombiningChar | Extender
 *
 * [5] Name ::= (Letter | '_' | ':') (NameChar)*
 *
 * [6] Names ::= Name (S Name)*
 *
 * Returns the Name parsed or NULL
 */

const xmlChar *
xmlParseName(xmlParserCtxtPtr ctxt) {
    const xmlChar *in;
    const xmlChar *ret;
    int count = 0;

    GROW;

    /*
     * Accelerator for simple ASCII names
     */
    in = ctxt->input->cur;
    if (((*in >= 0x61) && (*in <= 0x7A)) ||
	((*in >= 0x41) && (*in <= 0x5A)) ||
	(*in == '_') || (*in == ':')) {
	in++;
	while (((*in >= 0x61) && (*in <= 0x7A)) ||
	       ((*in >= 0x41) && (*in <= 0x5A)) ||
	       ((*in >= 0x30) && (*in <= 0x39)) ||
	       (*in == '_') || (*in == '-') ||
	       (*in == ':') || (*in == '.'))
	    in++;
	if ((*in > 0) && (*in < 0x80)) {
	    count = in - ctxt->input->cur;
	    ret = xmlDictLookup(ctxt->dict, ctxt->input->cur, count);
	    ctxt->input->cur = in;
	    ctxt->nbChars += count;
	    ctxt->input->col += count;
	    if (ret == NULL)
	        xmlErrMemory(ctxt, NULL);
	    return(ret);
	}
    }
    return(xmlParseNameComplex(ctxt));
}

/**
 * xmlParseNameAndCompare:
 * @ctxt:  an XML parser context
 *
 * parse an XML name and compares for match
 * (specialized for endtag parsing)
 *
 * Returns NULL for an illegal name, (xmlChar*) 1 for success
 * and the name for mismatch
 */

static const xmlChar *
xmlParseNameAndCompare(xmlParserCtxtPtr ctxt, xmlChar const *other) {
    const xmlChar *cmp = other;
    const xmlChar *in;
    const xmlChar *ret;

    GROW;
    
    in = ctxt->input->cur;
    while (*in != 0 && *in == *cmp) {
    	++in;
	++cmp;
    }
    if (*cmp == 0 && (*in == '>' || IS_BLANK_CH (*in))) {
    	/* success */
	ctxt->input->cur = in;
	return (const xmlChar*) 1;
    }
    /* failure (or end of input buffer), check with full function */
    ret = xmlParseName (ctxt);
    /* strings coming from the dictionnary direct compare possible */
    if (ret == other) {
	return (const xmlChar*) 1;
    }
    return ret;
}

static const xmlChar *
xmlParseNameComplex(xmlParserCtxtPtr ctxt) {
    int len = 0, l;
    int c;
    int count = 0;

    /*
     * Handler for more complex cases
     */
    GROW;
    c = CUR_CHAR(l);
    if ((c == ' ') || (c == '>') || (c == '/') || /* accelerators */
	(!IS_LETTER(c) && (c != '_') &&
         (c != ':'))) {
	return(NULL);
    }

    while ((c != ' ') && (c != '>') && (c != '/') && /* test bigname.xml */
	   ((IS_LETTER(c)) || (IS_DIGIT(c)) ||
            (c == '.') || (c == '-') ||
	    (c == '_') || (c == ':') || 
	    (IS_COMBINING(c)) ||
	    (IS_EXTENDER(c)))) {
	if (count++ > 100) {
	    count = 0;
	    GROW;
	}
	len += l;
	NEXTL(l);
	c = CUR_CHAR(l);
    }
    return(xmlDictLookup(ctxt->dict, ctxt->input->cur - len, len));
}

/**
 * xmlParseStringName:
 * @ctxt:  an XML parser context
 * @str:  a pointer to the string pointer (IN/OUT)
 *
 * parse an XML name.
 *
 * [4] NameChar ::= Letter | Digit | '.' | '-' | '_' | ':' |
 *                  CombiningChar | Extender
 *
 * [5] Name ::= (Letter | '_' | ':') (NameChar)*
 *
 * [6] Names ::= Name (S Name)*
 *
 * Returns the Name parsed or NULL. The @str pointer 
 * is updated to the current location in the string.
 */

static xmlChar *
xmlParseStringName(xmlParserCtxtPtr ctxt, const xmlChar** str) {
    xmlChar buf[XML_MAX_NAMELEN + 5];
    const xmlChar *cur = *str;
    int len = 0, l;
    int c;

    c = CUR_SCHAR(cur, l);
    if (!IS_LETTER(c) && (c != '_') &&
        (c != ':')) {
	return(NULL);
    }

    while ((IS_LETTER(c)) || (IS_DIGIT(c)) || /* test bigentname.xml */
           (c == '.') || (c == '-') ||
	   (c == '_') || (c == ':') || 
	   (IS_COMBINING(c)) ||
	   (IS_EXTENDER(c))) {
	COPY_BUF(l,buf,len,c);
	cur += l;
	c = CUR_SCHAR(cur, l);
	if (len >= XML_MAX_NAMELEN) { /* test bigentname.xml */
	    /*
	     * Okay someone managed to make a huge name, so he's ready to pay
	     * for the processing speed.
	     */
	    xmlChar *buffer;
	    int max = len * 2;
	    
	    buffer = (xmlChar *) xmlMallocAtomic(max * sizeof(xmlChar));
	    if (buffer == NULL) {
	        xmlErrMemory(ctxt, NULL);
		return(NULL);
	    }
	    memcpy(buffer, buf, len);
	    while ((IS_LETTER(c)) || (IS_DIGIT(c)) ||
	             /* test bigentname.xml */
		   (c == '.') || (c == '-') ||
		   (c == '_') || (c == ':') || 
		   (IS_COMBINING(c)) ||
		   (IS_EXTENDER(c))) {
		if (len + 10 > max) {
		    max *= 2;
		    buffer = (xmlChar *) xmlRealloc(buffer,
			                            max * sizeof(xmlChar));
		    if (buffer == NULL) {
			xmlErrMemory(ctxt, NULL);
			return(NULL);
		    }
		}
		COPY_BUF(l,buffer,len,c);
		cur += l;
		c = CUR_SCHAR(cur, l);
	    }
	    buffer[len] = 0;
	    *str = cur;
	    return(buffer);
	}
    }
    *str = cur;
    return(xmlStrndup(buf, len));
}

/**
 * xmlParseNmtoken:
 * @ctxt:  an XML parser context
 * 
 * parse an XML Nmtoken.
 *
 * [7] Nmtoken ::= (NameChar)+
 *
 * [8] Nmtokens ::= Nmtoken (S Nmtoken)*
 *
 * Returns the Nmtoken parsed or NULL
 */

xmlChar *
xmlParseNmtoken(xmlParserCtxtPtr ctxt) {
    xmlChar buf[XML_MAX_NAMELEN + 5];
    int len = 0, l;
    int c;
    int count = 0;

    GROW;
    c = CUR_CHAR(l);

    while ((IS_LETTER(c)) || (IS_DIGIT(c)) || /* test bigtoken.xml */
           (c == '.') || (c == '-') ||
	   (c == '_') || (c == ':') || 
	   (IS_COMBINING(c)) ||
	   (IS_EXTENDER(c))) {
	if (count++ > 100) {
	    count = 0;
	    GROW;
	}
	COPY_BUF(l,buf,len,c);
	NEXTL(l);
	c = CUR_CHAR(l);
	if (len >= XML_MAX_NAMELEN) {
	    /*
	     * Okay someone managed to make a huge token, so he's ready to pay
	     * for the processing speed.
	     */
	    xmlChar *buffer;
	    int max = len * 2;
	    
	    buffer = (xmlChar *) xmlMallocAtomic(max * sizeof(xmlChar));
	    if (buffer == NULL) {
	        xmlErrMemory(ctxt, NULL);
		return(NULL);
	    }
	    memcpy(buffer, buf, len);
	    while ((IS_LETTER(c)) || (IS_DIGIT(c)) || /* test bigtoken.xml */
		   (c == '.') || (c == '-') ||
		   (c == '_') || (c == ':') || 
		   (IS_COMBINING(c)) ||
		   (IS_EXTENDER(c))) {
		if (count++ > 100) {
		    count = 0;
		    GROW;
		}
		if (len + 10 > max) {
		    max *= 2;
		    buffer = (xmlChar *) xmlRealloc(buffer,
			                            max * sizeof(xmlChar));
		    if (buffer == NULL) {
			xmlErrMemory(ctxt, NULL);
			return(NULL);
		    }
		}
		COPY_BUF(l,buffer,len,c);
		NEXTL(l);
		c = CUR_CHAR(l);
	    }
	    buffer[len] = 0;
	    return(buffer);
	}
    }
    if (len == 0)
        return(NULL);
    return(xmlStrndup(buf, len));
}

/**
 * xmlParseEntityValue:
 * @ctxt:  an XML parser context
 * @orig:  if non-NULL store a copy of the original entity value
 *
 * parse a value for ENTITY declarations
 *
 * [9] EntityValue ::= '"' ([^%&"] | PEReference | Reference)* '"' |
 *	               "'" ([^%&'] | PEReference | Reference)* "'"
 *
 * Returns the EntityValue parsed with reference substituted or NULL
 */

xmlChar *
xmlParseEntityValue(xmlParserCtxtPtr ctxt, xmlChar **orig) {
    xmlChar *buf = NULL;
    int len = 0;
    int size = XML_PARSER_BUFFER_SIZE;
    int c, l;
    xmlChar stop;
    xmlChar *ret = NULL;
    const xmlChar *cur = NULL;
    xmlParserInputPtr input;

    if (RAW == '"') stop = '"';
    else if (RAW == '\'') stop = '\'';
    else {
	xmlFatalErr(ctxt, XML_ERR_ENTITY_NOT_STARTED, NULL);
	return(NULL);
    }
    buf = (xmlChar *) xmlMallocAtomic(size * sizeof(xmlChar));
    if (buf == NULL) {
	xmlErrMemory(ctxt, NULL);
	return(NULL);
    }

    /*
     * The content of the entity definition is copied in a buffer.
     */

    ctxt->instate = XML_PARSER_ENTITY_VALUE;
    input = ctxt->input;
    GROW;
    NEXT;
    c = CUR_CHAR(l);
    /*
     * NOTE: 4.4.5 Included in Literal
     * When a parameter entity reference appears in a literal entity
     * value, ... a single or double quote character in the replacement
     * text is always treated as a normal data character and will not
     * terminate the literal. 
     * In practice it means we stop the loop only when back at parsing
     * the initial entity and the quote is found
     */
    while ((IS_CHAR(c)) && ((c != stop) || /* checked */
	   (ctxt->input != input))) {
	if (len + 5 >= size) {
	    size *= 2;
	    buf = (xmlChar *) xmlRealloc(buf, size * sizeof(xmlChar));
	    if (buf == NULL) {
		xmlErrMemory(ctxt, NULL);
		return(NULL);
	    }
	}
	COPY_BUF(l,buf,len,c);
	NEXTL(l);
	/*
	 * Pop-up of finished entities.
	 */
	while ((RAW == 0) && (ctxt->inputNr > 1)) /* non input consuming */
	    xmlPopInput(ctxt);

	GROW;
	c = CUR_CHAR(l);
	if (c == 0) {
	    GROW;
	    c = CUR_CHAR(l);
	}
    }
    buf[len] = 0;

    /*
     * Raise problem w.r.t. '&' and '%' being used in non-entities
     * reference constructs. Note Charref will be handled in
     * xmlStringDecodeEntities()
     */
    cur = buf;
    while (*cur != 0) { /* non input consuming */
	if ((*cur == '%') || ((*cur == '&') && (cur[1] != '#'))) {
	    xmlChar *name;
	    xmlChar tmp = *cur;

	    cur++;
	    name = xmlParseStringName(ctxt, &cur);
            if ((name == NULL) || (*cur != ';')) {
		xmlFatalErrMsgInt(ctxt, XML_ERR_ENTITY_CHAR_ERROR,
	    "EntityValue: '%c' forbidden except for entities references\n",
	                          tmp);
	    }
	    if ((tmp == '%') && (ctxt->inSubset == 1) &&
		(ctxt->inputNr == 1)) {
		xmlFatalErr(ctxt, XML_ERR_ENTITY_PE_INTERNAL, NULL);
	    }
	    if (name != NULL)
		xmlFree(name);
	    if (*cur == 0)
	        break;
	}
	cur++;
    }

    /*
     * Then PEReference entities are substituted.
     */
    if (c != stop) {
	xmlFatalErr(ctxt, XML_ERR_ENTITY_NOT_FINISHED, NULL);
	xmlFree(buf);
    } else {
	NEXT;
	/*
	 * NOTE: 4.4.7 Bypassed
	 * When a general entity reference appears in the EntityValue in
	 * an entity declaration, it is bypassed and left as is.
	 * so XML_SUBSTITUTE_REF is not set here.
	 */
	ret = xmlStringDecodeEntities(ctxt, buf, XML_SUBSTITUTE_PEREF,
				      0, 0, 0);
	if (orig != NULL) 
	    *orig = buf;
	else
	    xmlFree(buf);
    }
    
    return(ret);
}

/**
 * xmlParseAttValueComplex:
 * @ctxt:  an XML parser context
 * @len:   the resulting attribute len
 * @normalize:  wether to apply the inner normalization
 *
 * parse a value for an attribute, this is the fallback function
 * of xmlParseAttValue() when the attribute parsing requires handling
 * of non-ASCII characters, or normalization compaction.
 *
 * Returns the AttValue parsed or NULL. The value has to be freed by the caller.
 */
static xmlChar *
xmlParseAttValueComplex(xmlParserCtxtPtr ctxt, int *attlen, int normalize) {
    xmlChar limit = 0;
    xmlChar *buf = NULL;
    int len = 0;
    int buf_size = 0;
    int c, l, in_space = 0;
    xmlChar *current = NULL;
    xmlEntityPtr ent;

    if (NXT(0) == '"') {
	ctxt->instate = XML_PARSER_ATTRIBUTE_VALUE;
	limit = '"';
        NEXT;
    } else if (NXT(0) == '\'') {
	limit = '\'';
	ctxt->instate = XML_PARSER_ATTRIBUTE_VALUE;
        NEXT;
    } else {
	xmlFatalErr(ctxt, XML_ERR_ATTRIBUTE_NOT_STARTED, NULL);
	return(NULL);
    }
    
    /*
     * allocate a translation buffer.
     */
    buf_size = XML_PARSER_BUFFER_SIZE;
    buf = (xmlChar *) xmlMallocAtomic(buf_size * sizeof(xmlChar));
    if (buf == NULL) goto mem_error;

    /*
     * OK loop until we reach one of the ending char or a size limit.
     */
    c = CUR_CHAR(l);
    while ((NXT(0) != limit) && /* checked */
	   (c != '<')) {
	if (c == 0) break;
	if (c == '&') {
	    in_space = 0;
	    if (NXT(1) == '#') {
		int val = xmlParseCharRef(ctxt);

		if (val == '&') {
		    if (ctxt->replaceEntities) {
			if (len > buf_size - 10) {
			    growBuffer(buf);
			}
			buf[len++] = '&';
		    } else {
			/*
			 * The reparsing will be done in xmlStringGetNodeList()
			 * called by the attribute() function in SAX.c
			 */
			if (len > buf_size - 10) {
			    growBuffer(buf);
			}
			buf[len++] = '&';
			buf[len++] = '#';
			buf[len++] = '3';
			buf[len++] = '8';
			buf[len++] = ';';
		    }
		} else {
		    if (len > buf_size - 10) {
			growBuffer(buf);
		    }
		    len += xmlCopyChar(0, &buf[len], val);
		}
	    } else {
		ent = xmlParseEntityRef(ctxt);
		if ((ent != NULL) &&
		    (ent->etype == XML_INTERNAL_PREDEFINED_ENTITY)) {
		    if (len > buf_size - 10) {
			growBuffer(buf);
		    }
		    if ((ctxt->replaceEntities == 0) &&
		        (ent->content[0] == '&')) {
			buf[len++] = '&';
			buf[len++] = '#';
			buf[len++] = '3';
			buf[len++] = '8';
			buf[len++] = ';';
		    } else {
			buf[len++] = ent->content[0];
		    }
		} else if ((ent != NULL) && 
		           (ctxt->replaceEntities != 0)) {
		    xmlChar *rep;

		    if (ent->etype != XML_INTERNAL_PREDEFINED_ENTITY) {
			rep = xmlStringDecodeEntities(ctxt, ent->content,
						      XML_SUBSTITUTE_REF,
						      0, 0, 0);
			if (rep != NULL) {
			    current = rep;
			    while (*current != 0) { /* non input consuming */
				buf[len++] = *current++;
				if (len > buf_size - 10) {
				    growBuffer(buf);
				}
			    }
			    xmlFree(rep);
			}
		    } else {
			if (len > buf_size - 10) {
			    growBuffer(buf);
			}
			if (ent->content != NULL)
			    buf[len++] = ent->content[0];
		    }
		} else if (ent != NULL) {
		    int i = xmlStrlen(ent->name);
		    const xmlChar *cur = ent->name;

		    /*
		     * This may look absurd but is needed to detect
		     * entities problems
		     */
		    if ((ent->etype != XML_INTERNAL_PREDEFINED_ENTITY) &&
			(ent->content != NULL)) {
			xmlChar *rep;
			rep = xmlStringDecodeEntities(ctxt, ent->content,
						      XML_SUBSTITUTE_REF, 0, 0, 0);
			if (rep != NULL)
			    xmlFree(rep);
		    }

		    /*
		     * Just output the reference
		     */
		    buf[len++] = '&';
		    if (len > buf_size - i - 10) {
			growBuffer(buf);
		    }
		    for (;i > 0;i--)
			buf[len++] = *cur++;
		    buf[len++] = ';';
		}
	    }
	} else {
	    if ((c == 0x20) || (c == 0xD) || (c == 0xA) || (c == 0x9)) {
	        if ((len != 0) || (!normalize)) {
		    if ((!normalize) || (!in_space)) {
			COPY_BUF(l,buf,len,0x20);
			if (len > buf_size - 10) {
			    growBuffer(buf);
			}
		    }
		    in_space = 1;
		}
	    } else {
	        in_space = 0;
		COPY_BUF(l,buf,len,c);
		if (len > buf_size - 10) {
		    growBuffer(buf);
		}
	    }
	    NEXTL(l);
	}
	GROW;
	c = CUR_CHAR(l);
    }
    if ((in_space) && (normalize)) {
        while (buf[len - 1] == 0x20) len--;
    }
    buf[len] = 0;
    if (RAW == '<') {
	xmlFatalErr(ctxt, XML_ERR_LT_IN_ATTRIBUTE, NULL);
    } else if (RAW != limit) {
        xmlFatalErrMsg(ctxt, XML_ERR_ATTRIBUTE_NOT_FINISHED,
	               "AttValue: ' expected\n");
    } else
	NEXT;
    if (attlen != NULL) *attlen = len;
    return(buf);

mem_error:
    xmlErrMemory(ctxt, NULL);
    return(NULL);
}

/**
 * xmlParseAttValue:
 * @ctxt:  an XML parser context
 *
 * parse a value for an attribute
 * Note: the parser won't do substitution of entities here, this
 * will be handled later in xmlStringGetNodeList
 *
 * [10] AttValue ::= '"' ([^<&"] | Reference)* '"' |
 *                   "'" ([^<&'] | Reference)* "'"
 *
 * 3.3.3 Attribute-Value Normalization:
 * Before the value of an attribute is passed to the application or
 * checked for validity, the XML processor must normalize it as follows: 
 * - a character reference is processed by appending the referenced
 *   character to the attribute value
 * - an entity reference is processed by recursively processing the
 *   replacement text of the entity 
 * - a whitespace character (#x20, #xD, #xA, #x9) is processed by
 *   appending #x20 to the normalized value, except that only a single
 *   #x20 is appended for a "#xD#xA" sequence that is part of an external
 *   parsed entity or the literal entity value of an internal parsed entity 
 * - other characters are processed by appending them to the normalized value 
 * If the declared value is not CDATA, then the XML processor must further
 * process the normalized attribute value by discarding any leading and
 * trailing space (#x20) characters, and by replacing sequences of space
 * (#x20) characters by a single space (#x20) character.  
 * All attributes for which no declaration has been read should be treated
 * by a non-validating parser as if declared CDATA.
 *
 * Returns the AttValue parsed or NULL. The value has to be freed by the caller.
 */


xmlChar *
xmlParseAttValue(xmlParserCtxtPtr ctxt) {
    return(xmlParseAttValueInternal(ctxt, NULL, NULL, 0));
}

/**
 * xmlParseSystemLiteral:
 * @ctxt:  an XML parser context
 * 
 * parse an XML Literal
 *
 * [11] SystemLiteral ::= ('"' [^"]* '"') | ("'" [^']* "'")
 *
 * Returns the SystemLiteral parsed or NULL
 */

xmlChar *
xmlParseSystemLiteral(xmlParserCtxtPtr ctxt) {
    xmlChar *buf = NULL;
    int len = 0;
    int size = XML_PARSER_BUFFER_SIZE;
    int cur, l;
    xmlChar stop;
    int state = ctxt->instate;
    int count = 0;

    SHRINK;
    if (RAW == '"') {
        NEXT;
	stop = '"';
    } else if (RAW == '\'') {
        NEXT;
	stop = '\'';
    } else {
	xmlFatalErr(ctxt, XML_ERR_LITERAL_NOT_STARTED, NULL);
	return(NULL);
    }
    
    buf = (xmlChar *) xmlMallocAtomic(size * sizeof(xmlChar));
    if (buf == NULL) {
        xmlErrMemory(ctxt, NULL);
	return(NULL);
    }
    ctxt->instate = XML_PARSER_SYSTEM_LITERAL;
    cur = CUR_CHAR(l);
    while ((IS_CHAR(cur)) && (cur != stop)) { /* checked */
	if (len + 5 >= size) {
	    size *= 2;
	    buf = (xmlChar *) xmlRealloc(buf, size * sizeof(xmlChar));
	    if (buf == NULL) {
		xmlErrMemory(ctxt, NULL);
		ctxt->instate = (xmlParserInputState) state;
		return(NULL);
	    }
	}
	count++;
	if (count > 50) {
	    GROW;
	    count = 0;
	}
	COPY_BUF(l,buf,len,cur);
	NEXTL(l);
	cur = CUR_CHAR(l);
	if (cur == 0) {
	    GROW;
	    SHRINK;
	    cur = CUR_CHAR(l);
	}
    }
    buf[len] = 0;
    ctxt->instate = (xmlParserInputState) state;
    if (!IS_CHAR(cur)) {
	xmlFatalErr(ctxt, XML_ERR_LITERAL_NOT_FINISHED, NULL);
    } else {
	NEXT;
    }
    return(buf);
}

/**
 * xmlParsePubidLiteral:
 * @ctxt:  an XML parser context
 *
 * parse an XML public literal
 *
 * [12] PubidLiteral ::= '"' PubidChar* '"' | "'" (PubidChar - "'")* "'"
 *
 * Returns the PubidLiteral parsed or NULL.
 */

xmlChar *
xmlParsePubidLiteral(xmlParserCtxtPtr ctxt) {
    xmlChar *buf = NULL;
    int len = 0;
    int size = XML_PARSER_BUFFER_SIZE;
    xmlChar cur;
    xmlChar stop;
    int count = 0;
    xmlParserInputState oldstate = ctxt->instate;

    SHRINK;
    if (RAW == '"') {
        NEXT;
	stop = '"';
    } else if (RAW == '\'') {
        NEXT;
	stop = '\'';
    } else {
	xmlFatalErr(ctxt, XML_ERR_LITERAL_NOT_STARTED, NULL);
	return(NULL);
    }
    buf = (xmlChar *) xmlMallocAtomic(size * sizeof(xmlChar));
    if (buf == NULL) {
	xmlErrMemory(ctxt, NULL);
	return(NULL);
    }
    ctxt->instate = XML_PARSER_PUBLIC_LITERAL;
    cur = CUR;
    while ((IS_PUBIDCHAR_CH(cur)) && (cur != stop)) { /* checked */
	if (len + 1 >= size) {
	    size *= 2;
	    buf = (xmlChar *) xmlRealloc(buf, size * sizeof(xmlChar));
	    if (buf == NULL) {
		xmlErrMemory(ctxt, NULL);
		return(NULL);
	    }
	}
	buf[len++] = cur;
	count++;
	if (count > 50) {
	    GROW;
	    count = 0;
	}
	NEXT;
	cur = CUR;
	if (cur == 0) {
	    GROW;
	    SHRINK;
	    cur = CUR;
	}
    }
    buf[len] = 0;
    if (cur != stop) {
	xmlFatalErr(ctxt, XML_ERR_LITERAL_NOT_FINISHED, NULL);
    } else {
	NEXT;
    }
    ctxt->instate = oldstate;
    return(buf);
}

void xmlParseCharDataComplex(xmlParserCtxtPtr ctxt, int cdata);
/**
 * xmlParseCharData:
 * @ctxt:  an XML parser context
 * @cdata:  int indicating whether we are within a CDATA section
 *
 * parse a CharData section.
 * if we are within a CDATA section ']]>' marks an end of section.
 *
 * The right angle bracket (>) may be represented using the string "&gt;",
 * and must, for compatibility, be escaped using "&gt;" or a character
 * reference when it appears in the string "]]>" in content, when that
 * string is not marking the end of a CDATA section. 
 *
 * [14] CharData ::= [^<&]* - ([^<&]* ']]>' [^<&]*)
 */

void
xmlParseCharData(xmlParserCtxtPtr ctxt, int cdata) {
    const xmlChar *in;
    int nbchar = 0;
    int line = ctxt->input->line;
    int col = ctxt->input->col;

    SHRINK;
    GROW;
    /*
     * Accelerated common case where input don't need to be
     * modified before passing it to the handler.
     */
    if (!cdata) {
	in = ctxt->input->cur;
	do {
get_more:
	    while (((*in >= 0x20) && (*in != '<') && (*in != ']') &&
		    (*in != '&') && (*in <= 0x7F)) || (*in == 0x09))
		in++;
	    if (*in == 0xA) {
		ctxt->input->line++;
		in++;
		while (*in == 0xA) {
		    ctxt->input->line++;
		    in++;
		}
		goto get_more;
	    }
	    if (*in == ']') {
		if ((in[1] == ']') && (in[2] == '>')) {
		    xmlFatalErr(ctxt, XML_ERR_MISPLACED_CDATA_END, NULL);
		    ctxt->input->cur = in;
		    return;
		}
		in++;
		goto get_more;
	    }
	    nbchar = in - ctxt->input->cur;
	    if (nbchar > 0) {
		if ((ctxt->sax->ignorableWhitespace !=
		     ctxt->sax->characters) &&
		    (IS_BLANK_CH(*ctxt->input->cur))) {
		    const xmlChar *tmp = ctxt->input->cur;
		    ctxt->input->cur = in;

		    if (areBlanks(ctxt, tmp, nbchar)) {
			ctxt->sax->ignorableWhitespace(ctxt->userData,
					       tmp, nbchar);
		    } else if (ctxt->sax->characters != NULL)
			ctxt->sax->characters(ctxt->userData,
					      tmp, nbchar);
                    line = ctxt->input->line;
                    col = ctxt->input->col;
		} else {
		    if (ctxt->sax->characters != NULL)
			ctxt->sax->characters(ctxt->userData,
					      ctxt->input->cur, nbchar);
                    line = ctxt->input->line;
                    col = ctxt->input->col;
		}
	    }
	    ctxt->input->cur = in;
	    if (*in == 0xD) {
		in++;
		if (*in == 0xA) {
		    ctxt->input->cur = in;
		    in++;
		    ctxt->input->line++;
		    continue; /* while */
		}
		in--;
	    }
	    if (*in == '<') {
		return;
	    }
	    if (*in == '&') {
		return;
	    }
	    SHRINK;
	    GROW;
	    in = ctxt->input->cur;
	} while (((*in >= 0x20) && (*in <= 0x7F)) || (*in == 0x09));
	nbchar = 0;
    }
    ctxt->input->line = line;
    ctxt->input->col = col;
    xmlParseCharDataComplex(ctxt, cdata);
}

/**
 * xmlParseCharDataComplex:
 * @ctxt:  an XML parser context
 * @cdata:  int indicating whether we are within a CDATA section
 *
 * parse a CharData section.this is the fallback function
 * of xmlParseCharData() when the parsing requires handling
 * of non-ASCII characters.
 */
void
xmlParseCharDataComplex(xmlParserCtxtPtr ctxt, int cdata) {
    xmlChar buf[XML_PARSER_BIG_BUFFER_SIZE + 5];
    int nbchar = 0;
    int cur, l;
    int count = 0;

    SHRINK;
    GROW;
    cur = CUR_CHAR(l);
    while ((cur != '<') && /* checked */
           (cur != '&') && 
	   (IS_CHAR(cur))) /* test also done in xmlCurrentChar() */ {
	if ((cur == ']') && (NXT(1) == ']') &&
	    (NXT(2) == '>')) {
	    if (cdata) break;
	    else {
		xmlFatalErr(ctxt, XML_ERR_MISPLACED_CDATA_END, NULL);
	    }
	}
	COPY_BUF(l,buf,nbchar,cur);
	if (nbchar >= XML_PARSER_BIG_BUFFER_SIZE) {
	    buf[nbchar] = 0;

	    /*
	     * OK the segment is to be consumed as chars.
	     */
	    if ((ctxt->sax != NULL) && (!ctxt->disableSAX)) {
		if (areBlanks(ctxt, buf, nbchar)) {
		    if (ctxt->sax->ignorableWhitespace != NULL)
			ctxt->sax->ignorableWhitespace(ctxt->userData,
			                               buf, nbchar);
		} else {
		    if (ctxt->sax->characters != NULL)
			ctxt->sax->characters(ctxt->userData, buf, nbchar);
		}
	    }
	    nbchar = 0;
	}
	count++;
	if (count > 50) {
	    GROW;
	    count = 0;
	}
	NEXTL(l);
	cur = CUR_CHAR(l);
    }
    if (nbchar != 0) {
        buf[nbchar] = 0;
	/*
	 * OK the segment is to be consumed as chars.
	 */
	if ((ctxt->sax != NULL) && (!ctxt->disableSAX)) {
	    if (areBlanks(ctxt, buf, nbchar)) {
		if (ctxt->sax->ignorableWhitespace != NULL)
		    ctxt->sax->ignorableWhitespace(ctxt->userData, buf, nbchar);
	    } else {
		if (ctxt->sax->characters != NULL)
		    ctxt->sax->characters(ctxt->userData, buf, nbchar);
	    }
	}
    }
}

/**
 * xmlParseExternalID:
 * @ctxt:  an XML parser context
 * @publicID:  a xmlChar** receiving PubidLiteral
 * @strict: indicate whether we should restrict parsing to only
 *          production [75], see NOTE below
 *
 * Parse an External ID or a Public ID
 *
 * NOTE: Productions [75] and [83] interact badly since [75] can generate
 *       'PUBLIC' S PubidLiteral S SystemLiteral
 *
 * [75] ExternalID ::= 'SYSTEM' S SystemLiteral
 *                   | 'PUBLIC' S PubidLiteral S SystemLiteral
 *
 * [83] PublicID ::= 'PUBLIC' S PubidLiteral
 *
 * Returns the function returns SystemLiteral and in the second
 *                case publicID receives PubidLiteral, is strict is off
 *                it is possible to return NULL and have publicID set.
 */

xmlChar *
xmlParseExternalID(xmlParserCtxtPtr ctxt, xmlChar **publicID, int strict) {
    xmlChar *URI = NULL;

    SHRINK;

    *publicID = NULL;
    if (CMP6(CUR_PTR, 'S', 'Y', 'S', 'T', 'E', 'M')) {
        SKIP(6);
	if (!IS_BLANK_CH(CUR)) {
	    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
	                   "Space required after 'SYSTEM'\n");
	}
        SKIP_BLANKS;
	URI = xmlParseSystemLiteral(ctxt);
	if (URI == NULL) {
	    xmlFatalErr(ctxt, XML_ERR_URI_REQUIRED, NULL);
        }
    } else if (CMP6(CUR_PTR, 'P', 'U', 'B', 'L', 'I', 'C')) {
        SKIP(6);
	if (!IS_BLANK_CH(CUR)) {
	    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
		    "Space required after 'PUBLIC'\n");
	}
        SKIP_BLANKS;
	*publicID = xmlParsePubidLiteral(ctxt);
	if (*publicID == NULL) {
	    xmlFatalErr(ctxt, XML_ERR_PUBID_REQUIRED, NULL);
	}
	if (strict) {
	    /*
	     * We don't handle [83] so "S SystemLiteral" is required.
	     */
	    if (!IS_BLANK_CH(CUR)) {
		xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
			"Space required after the Public Identifier\n");
	    }
	} else {
	    /*
	     * We handle [83] so we return immediately, if 
	     * "S SystemLiteral" is not detected. From a purely parsing
	     * point of view that's a nice mess.
	     */
	    const xmlChar *ptr;
	    GROW;

	    ptr = CUR_PTR;
	    if (!IS_BLANK_CH(*ptr)) return(NULL);
	    
	    while (IS_BLANK_CH(*ptr)) ptr++; /* TODO: dangerous, fix ! */
	    if ((*ptr != '\'') && (*ptr != '"')) return(NULL);
	}
        SKIP_BLANKS;
	URI = xmlParseSystemLiteral(ctxt);
	if (URI == NULL) {
	    xmlFatalErr(ctxt, XML_ERR_URI_REQUIRED, NULL);
        }
    }
    return(URI);
}

/**
 * xmlParseComment:
 * @ctxt:  an XML parser context
 *
 * Skip an XML (SGML) comment <!-- .... -->
 *  The spec says that "For compatibility, the string "--" (double-hyphen)
 *  must not occur within comments. "
 *
 * [15] Comment ::= '<!--' ((Char - '-') | ('-' (Char - '-')))* '-->'
 */
void
xmlParseComment(xmlParserCtxtPtr ctxt) {
    xmlChar *buf = NULL;
    int len;
    int size = XML_PARSER_BUFFER_SIZE;
    int q, ql;
    int r, rl;
    int cur, l;
    xmlParserInputState state;
    xmlParserInputPtr input = ctxt->input;
    int count = 0;

    /*
     * Check that there is a comment right here.
     */
    if ((RAW != '<') || (NXT(1) != '!') ||
        (NXT(2) != '-') || (NXT(3) != '-')) return;

    state = ctxt->instate;
    ctxt->instate = XML_PARSER_COMMENT;
    SHRINK;
    SKIP(4);
    buf = (xmlChar *) xmlMallocAtomic(size * sizeof(xmlChar));
    if (buf == NULL) {
        xmlErrMemory(ctxt, NULL);
	ctxt->instate = state;
	return;
    }
    q = CUR_CHAR(ql);
    if (q == 0)
        goto not_terminated;
    NEXTL(ql);
    r = CUR_CHAR(rl);
    if (r == 0)
        goto not_terminated;
    NEXTL(rl);
    cur = CUR_CHAR(l);
    if (cur == 0)
        goto not_terminated;
    len = 0;
    while (IS_CHAR(cur) && /* checked */
           ((cur != '>') ||
	    (r != '-') || (q != '-'))) {
	if ((r == '-') && (q == '-')) {
	    xmlFatalErr(ctxt, XML_ERR_HYPHEN_IN_COMMENT, NULL);
	}
	if (len + 5 >= size) {
	    size *= 2;
	    buf = (xmlChar *) xmlRealloc(buf, size * sizeof(xmlChar));
	    if (buf == NULL) {
		xmlErrMemory(ctxt, NULL);
		ctxt->instate = state;
		return;
	    }
	}
	COPY_BUF(ql,buf,len,q);
	q = r;
	ql = rl;
	r = cur;
	rl = l;

	count++;
	if (count > 50) {
	    GROW;
	    count = 0;
	}
	NEXTL(l);
	cur = CUR_CHAR(l);
	if (cur == 0) {
	    SHRINK;
	    GROW;
	    cur = CUR_CHAR(l);
	}
    }
    buf[len] = 0;
    if (!IS_CHAR(cur)) {
	xmlFatalErrMsgStr(ctxt, XML_ERR_COMMENT_NOT_FINISHED,
	                     "Comment not terminated \n<!--%.50s\n", buf);
	xmlFree(buf);
    } else {
	if (input != ctxt->input) {
	    xmlFatalErrMsg(ctxt, XML_ERR_ENTITY_BOUNDARY,
		"Comment doesn't start and stop in the same entity\n");
	}
        NEXT;
	if ((ctxt->sax != NULL) && (ctxt->sax->comment != NULL) &&
	    (!ctxt->disableSAX))
	    ctxt->sax->comment(ctxt->userData, buf);
	xmlFree(buf);
    }
    ctxt->instate = state;
    return;
not_terminated:
    xmlFatalErrMsgStr(ctxt, XML_ERR_COMMENT_NOT_FINISHED,
			 "Comment not terminated\n", NULL);
    xmlFree(buf);
}

/**
 * xmlParsePITarget:
 * @ctxt:  an XML parser context
 * 
 * parse the name of a PI
 *
 * [17] PITarget ::= Name - (('X' | 'x') ('M' | 'm') ('L' | 'l'))
 *
 * Returns the PITarget name or NULL
 */

const xmlChar *
xmlParsePITarget(xmlParserCtxtPtr ctxt) {
    const xmlChar *name;

    name = xmlParseName(ctxt);
    if ((name != NULL) &&
        ((name[0] == 'x') || (name[0] == 'X')) &&
        ((name[1] == 'm') || (name[1] == 'M')) &&
        ((name[2] == 'l') || (name[2] == 'L'))) {
	int i;
	if ((name[0] == 'x') && (name[1] == 'm') &&
	    (name[2] == 'l') && (name[3] == 0)) {
	    xmlFatalErrMsg(ctxt, XML_ERR_RESERVED_XML_NAME,
		 "XML declaration allowed only at the start of the document\n");
	    return(name);
	} else if (name[3] == 0) {
	    xmlFatalErr(ctxt, XML_ERR_RESERVED_XML_NAME, NULL);
	    return(name);
	}
	for (i = 0;;i++) {
	    if (xmlW3CPIs[i] == NULL) break;
	    if (xmlStrEqual(name, (const xmlChar *)xmlW3CPIs[i]))
	        return(name);
	}
	xmlWarningMsg(ctxt, XML_ERR_RESERVED_XML_NAME,
		      "xmlParsePITarget: invalid name prefix 'xml'\n",
		      NULL, NULL);
    }
    return(name);
}

#ifdef LIBXML_CATALOG_ENABLED
/**
 * xmlParseCatalogPI:
 * @ctxt:  an XML parser context
 * @catalog:  the PI value string
 * 
 * parse an XML Catalog Processing Instruction.
 *
 * <?oasis-xml-catalog catalog="http://example.com/catalog.xml"?>
 *
 * Occurs only if allowed by the user and if happening in the Misc
 * part of the document before any doctype informations
 * This will add the given catalog to the parsing context in order
 * to be used if there is a resolution need further down in the document
 */

static void
xmlParseCatalogPI(xmlParserCtxtPtr ctxt, const xmlChar *catalog) {
    xmlChar *URL = NULL;
    const xmlChar *tmp, *base;
    xmlChar marker;

    tmp = catalog;
    while (IS_BLANK_CH(*tmp)) tmp++;
    if (xmlStrncmp(tmp, BAD_CAST"catalog", 7))
	goto error;
    tmp += 7;
    while (IS_BLANK_CH(*tmp)) tmp++;
    if (*tmp != '=') {
	return;
    }
    tmp++;
    while (IS_BLANK_CH(*tmp)) tmp++;
    marker = *tmp;
    if ((marker != '\'') && (marker != '"'))
	goto error;
    tmp++;
    base = tmp;
    while ((*tmp != 0) && (*tmp != marker)) tmp++;
    if (*tmp == 0)
	goto error;
    URL = xmlStrndup(base, tmp - base);
    tmp++;
    while (IS_BLANK_CH(*tmp)) tmp++;
    if (*tmp != 0)
	goto error;

    if (URL != NULL) {
	ctxt->catalogs = xmlCatalogAddLocal(ctxt->catalogs, URL);
	xmlFree(URL);
    }
    return;

error:
    xmlWarningMsg(ctxt, XML_WAR_CATALOG_PI,
	          "Catalog PI syntax error: %s\n",
		  catalog, NULL);
    if (URL != NULL)
	xmlFree(URL);
}
#endif

/**
 * xmlParsePI:
 * @ctxt:  an XML parser context
 * 
 * parse an XML Processing Instruction.
 *
 * [16] PI ::= '<?' PITarget (S (Char* - (Char* '?>' Char*)))? '?>'
 *
 * The processing is transfered to SAX once parsed.
 */

void
xmlParsePI(xmlParserCtxtPtr ctxt) {
    xmlChar *buf = NULL;
    int len = 0;
    int size = XML_PARSER_BUFFER_SIZE;
    int cur, l;
    const xmlChar *target;
    xmlParserInputState state;
    int count = 0;

    if ((RAW == '<') && (NXT(1) == '?')) {
	xmlParserInputPtr input = ctxt->input;
	state = ctxt->instate;
        ctxt->instate = XML_PARSER_PI;
	/*
	 * this is a Processing Instruction.
	 */
	SKIP(2);
	SHRINK;

	/*
	 * Parse the target name and check for special support like
	 * namespace.
	 */
        target = xmlParsePITarget(ctxt);
	if (target != NULL) {
	    if ((RAW == '?') && (NXT(1) == '>')) {
		if (input != ctxt->input) {
		    xmlFatalErrMsg(ctxt, XML_ERR_ENTITY_BOUNDARY,
	    "PI declaration doesn't start and stop in the same entity\n");
		}
		SKIP(2);

		/*
		 * SAX: PI detected.
		 */
		if ((ctxt->sax) && (!ctxt->disableSAX) &&
		    (ctxt->sax->processingInstruction != NULL))
		    ctxt->sax->processingInstruction(ctxt->userData,
		                                     target, NULL);
		ctxt->instate = state;
		return;
	    }
	    buf = (xmlChar *) xmlMallocAtomic(size * sizeof(xmlChar));
	    if (buf == NULL) {
		xmlErrMemory(ctxt, NULL);
		ctxt->instate = state;
		return;
	    }
	    cur = CUR;
	    if (!IS_BLANK(cur)) {
		xmlFatalErrMsgStr(ctxt, XML_ERR_SPACE_REQUIRED,
			  "ParsePI: PI %s space expected\n", target);
	    }
            SKIP_BLANKS;
	    cur = CUR_CHAR(l);
	    while (IS_CHAR(cur) && /* checked */
		   ((cur != '?') || (NXT(1) != '>'))) {
		if (len + 5 >= size) {
		    size *= 2;
		    buf = (xmlChar *) xmlRealloc(buf, size * sizeof(xmlChar));
		    if (buf == NULL) {
			xmlErrMemory(ctxt, NULL);
			ctxt->instate = state;
			return;
		    }
		}
		count++;
		if (count > 50) {
		    GROW;
		    count = 0;
		}
		COPY_BUF(l,buf,len,cur);
		NEXTL(l);
		cur = CUR_CHAR(l);
		if (cur == 0) {
		    SHRINK;
		    GROW;
		    cur = CUR_CHAR(l);
		}
	    }
	    buf[len] = 0;
	    if (cur != '?') {
		xmlFatalErrMsgStr(ctxt, XML_ERR_PI_NOT_FINISHED,
		      "ParsePI: PI %s never end ...\n", target);
	    } else {
		if (input != ctxt->input) {
		    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
	    "PI declaration doesn't start and stop in the same entity\n");
		}
		SKIP(2);

#ifdef LIBXML_CATALOG_ENABLED
		if (((state == XML_PARSER_MISC) ||
	             (state == XML_PARSER_START)) &&
		    (xmlStrEqual(target, XML_CATALOG_PI))) {
		    xmlCatalogAllow allow = xmlCatalogGetDefaults();
		    if ((allow == XML_CATA_ALLOW_DOCUMENT) ||
			(allow == XML_CATA_ALLOW_ALL))
			xmlParseCatalogPI(ctxt, buf);
		}
#endif


		/*
		 * SAX: PI detected.
		 */
		if ((ctxt->sax) && (!ctxt->disableSAX) &&
		    (ctxt->sax->processingInstruction != NULL))
		    ctxt->sax->processingInstruction(ctxt->userData,
		                                     target, buf);
	    }
	    xmlFree(buf);
	} else {
	    xmlFatalErr(ctxt, XML_ERR_PI_NOT_STARTED, NULL);
	}
	ctxt->instate = state;
    }
}

/**
 * xmlParseNotationDecl:
 * @ctxt:  an XML parser context
 *
 * parse a notation declaration
 *
 * [82] NotationDecl ::= '<!NOTATION' S Name S (ExternalID |  PublicID) S? '>'
 *
 * Hence there is actually 3 choices:
 *     'PUBLIC' S PubidLiteral
 *     'PUBLIC' S PubidLiteral S SystemLiteral
 * and 'SYSTEM' S SystemLiteral
 *
 * See the NOTE on xmlParseExternalID().
 */

void
xmlParseNotationDecl(xmlParserCtxtPtr ctxt) {
    const xmlChar *name;
    xmlChar *Pubid;
    xmlChar *Systemid;
    
    if (CMP10(CUR_PTR, '<', '!', 'N', 'O', 'T', 'A', 'T', 'I', 'O', 'N')) {
	xmlParserInputPtr input = ctxt->input;
	SHRINK;
	SKIP(10);
	if (!IS_BLANK_CH(CUR)) {
	    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
			   "Space required after '<!NOTATION'\n");
	    return;
	}
	SKIP_BLANKS;

        name = xmlParseName(ctxt);
	if (name == NULL) {
	    xmlFatalErr(ctxt, XML_ERR_NOTATION_NOT_STARTED, NULL);
	    return;
	}
	if (!IS_BLANK_CH(CUR)) {
	    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
		     "Space required after the NOTATION name'\n");
	    return;
	}
	SKIP_BLANKS;

	/*
	 * Parse the IDs.
	 */
	Systemid = xmlParseExternalID(ctxt, &Pubid, 0);
	SKIP_BLANKS;

	if (RAW == '>') {
	    if (input != ctxt->input) {
		xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
	"Notation declaration doesn't start and stop in the same entity\n");
	    }
	    NEXT;
	    if ((ctxt->sax != NULL) && (!ctxt->disableSAX) &&
		(ctxt->sax->notationDecl != NULL))
		ctxt->sax->notationDecl(ctxt->userData, name, Pubid, Systemid);
	} else {
	    xmlFatalErr(ctxt, XML_ERR_NOTATION_NOT_FINISHED, NULL);
	}
	if (Systemid != NULL) xmlFree(Systemid);
	if (Pubid != NULL) xmlFree(Pubid);
    }
}

/**
 * xmlParseEntityDecl:
 * @ctxt:  an XML parser context
 *
 * parse <!ENTITY declarations
 *
 * [70] EntityDecl ::= GEDecl | PEDecl
 *
 * [71] GEDecl ::= '<!ENTITY' S Name S EntityDef S? '>'
 *
 * [72] PEDecl ::= '<!ENTITY' S '%' S Name S PEDef S? '>'
 *
 * [73] EntityDef ::= EntityValue | (ExternalID NDataDecl?)
 *
 * [74] PEDef ::= EntityValue | ExternalID
 *
 * [76] NDataDecl ::= S 'NDATA' S Name
 *
 * [ VC: Notation Declared ]
 * The Name must match the declared name of a notation.
 */

void
xmlParseEntityDecl(xmlParserCtxtPtr ctxt) {
    const xmlChar *name = NULL;
    xmlChar *value = NULL;
    xmlChar *URI = NULL, *literal = NULL;
    const xmlChar *ndata = NULL;
    int isParameter = 0;
    xmlChar *orig = NULL;
    int skipped;
    
    GROW;
    if (CMP8(CUR_PTR, '<', '!', 'E', 'N', 'T', 'I', 'T', 'Y')) {
	xmlParserInputPtr input = ctxt->input;
	SHRINK;
	SKIP(8);
	skipped = SKIP_BLANKS;
	if (skipped == 0) {
	    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
			   "Space required after '<!ENTITY'\n");
	}

	if (RAW == '%') {
	    NEXT;
	    skipped = SKIP_BLANKS;
	    if (skipped == 0) {
		xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
			       "Space required after '%'\n");
	    }
	    isParameter = 1;
	}

        name = xmlParseName(ctxt);
	if (name == NULL) {
	    xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED,
	                   "xmlParseEntityDecl: no name\n");
            return;
	}
        skipped = SKIP_BLANKS;
	if (skipped == 0) {
	    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
			   "Space required after the entity name\n");
	}

	ctxt->instate = XML_PARSER_ENTITY_DECL;
	/*
	 * handle the various case of definitions...
	 */
	if (isParameter) {
	    if ((RAW == '"') || (RAW == '\'')) {
	        value = xmlParseEntityValue(ctxt, &orig);
		if (value) {
		    if ((ctxt->sax != NULL) &&
			(!ctxt->disableSAX) && (ctxt->sax->entityDecl != NULL))
			ctxt->sax->entityDecl(ctxt->userData, name,
		                    XML_INTERNAL_PARAMETER_ENTITY,
				    NULL, NULL, value);
		}
	    } else {
	        URI = xmlParseExternalID(ctxt, &literal, 1);
		if ((URI == NULL) && (literal == NULL)) {
		    xmlFatalErr(ctxt, XML_ERR_VALUE_REQUIRED, NULL);
		}
		if (URI) {
		    xmlURIPtr uri;

		    uri = xmlParseURI((const char *) URI);
		    if (uri == NULL) {
		        xmlErrMsgStr(ctxt, XML_ERR_INVALID_URI,
				     "Invalid URI: %s\n", URI);
			/*
			 * This really ought to be a well formedness error
			 * but the XML Core WG decided otherwise c.f. issue
			 * E26 of the XML erratas.
			 */
		    } else {
			if (uri->fragment != NULL) {
			    /*
			     * Okay this is foolish to block those but not
			     * invalid URIs.
			     */
			    xmlFatalErr(ctxt, XML_ERR_URI_FRAGMENT, NULL);
			} else {
			    if ((ctxt->sax != NULL) &&
				(!ctxt->disableSAX) &&
				(ctxt->sax->entityDecl != NULL))
				ctxt->sax->entityDecl(ctxt->userData, name,
					    XML_EXTERNAL_PARAMETER_ENTITY,
					    literal, URI, NULL);
			}
			xmlFreeURI(uri);
		    }
		}
	    }
	} else {
	    if ((RAW == '"') || (RAW == '\'')) {
	        value = xmlParseEntityValue(ctxt, &orig);
		if ((ctxt->sax != NULL) &&
		    (!ctxt->disableSAX) && (ctxt->sax->entityDecl != NULL))
		    ctxt->sax->entityDecl(ctxt->userData, name,
				XML_INTERNAL_GENERAL_ENTITY,
				NULL, NULL, value);
		/*
		 * For expat compatibility in SAX mode.
		 */
		if ((ctxt->myDoc == NULL) ||
		    (xmlStrEqual(ctxt->myDoc->version, SAX_COMPAT_MODE))) {
		    if (ctxt->myDoc == NULL) {
			ctxt->myDoc = xmlNewDoc(SAX_COMPAT_MODE);
		    }
		    if (ctxt->myDoc->intSubset == NULL)
			ctxt->myDoc->intSubset = xmlNewDtd(ctxt->myDoc,
					    BAD_CAST "fake", NULL, NULL);

		    xmlSAX2EntityDecl(ctxt, name, XML_INTERNAL_GENERAL_ENTITY,
			              NULL, NULL, value);
		}
	    } else {
	        URI = xmlParseExternalID(ctxt, &literal, 1);
		if ((URI == NULL) && (literal == NULL)) {
		    xmlFatalErr(ctxt, XML_ERR_VALUE_REQUIRED, NULL);
		}
		if (URI) {
		    xmlURIPtr uri;

		    uri = xmlParseURI((const char *)URI);
		    if (uri == NULL) {
		        xmlErrMsgStr(ctxt, XML_ERR_INVALID_URI,
				     "Invalid URI: %s\n", URI);
			/*
			 * This really ought to be a well formedness error
			 * but the XML Core WG decided otherwise c.f. issue
			 * E26 of the XML erratas.
			 */
		    } else {
			if (uri->fragment != NULL) {
			    /*
			     * Okay this is foolish to block those but not
			     * invalid URIs.
			     */
			    xmlFatalErr(ctxt, XML_ERR_URI_FRAGMENT, NULL);
			}
			xmlFreeURI(uri);
		    }
		}
		if ((RAW != '>') && (!IS_BLANK_CH(CUR))) {
		    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
				   "Space required before 'NDATA'\n");
		}
		SKIP_BLANKS;
		if (CMP5(CUR_PTR, 'N', 'D', 'A', 'T', 'A')) {
		    SKIP(5);
		    if (!IS_BLANK_CH(CUR)) {
			xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
				       "Space required after 'NDATA'\n");
		    }
		    SKIP_BLANKS;
		    ndata = xmlParseName(ctxt);
		    if ((ctxt->sax != NULL) && (!ctxt->disableSAX) &&
		        (ctxt->sax->unparsedEntityDecl != NULL))
			ctxt->sax->unparsedEntityDecl(ctxt->userData, name,
				    literal, URI, ndata);
		} else {
		    if ((ctxt->sax != NULL) &&
		        (!ctxt->disableSAX) && (ctxt->sax->entityDecl != NULL))
			ctxt->sax->entityDecl(ctxt->userData, name,
				    XML_EXTERNAL_GENERAL_PARSED_ENTITY,
				    literal, URI, NULL);
		    /*
		     * For expat compatibility in SAX mode.
		     * assuming the entity repalcement was asked for
		     */
		    if ((ctxt->replaceEntities != 0) &&
			((ctxt->myDoc == NULL) ||
			(xmlStrEqual(ctxt->myDoc->version, SAX_COMPAT_MODE)))) {
			if (ctxt->myDoc == NULL) {
			    ctxt->myDoc = xmlNewDoc(SAX_COMPAT_MODE);
			}

			if (ctxt->myDoc->intSubset == NULL)
			    ctxt->myDoc->intSubset = xmlNewDtd(ctxt->myDoc,
						BAD_CAST "fake", NULL, NULL);
			xmlSAX2EntityDecl(ctxt, name,
				          XML_EXTERNAL_GENERAL_PARSED_ENTITY,
				          literal, URI, NULL);
		    }
		}
	    }
	}
	SKIP_BLANKS;
	if (RAW != '>') {
	    xmlFatalErrMsgStr(ctxt, XML_ERR_ENTITY_NOT_FINISHED,
	            "xmlParseEntityDecl: entity %s not terminated\n", name);
	} else {
	    if (input != ctxt->input) {
		xmlFatalErrMsg(ctxt, XML_ERR_ENTITY_BOUNDARY,
	"Entity declaration doesn't start and stop in the same entity\n");
	    }
	    NEXT;
	}
	if (orig != NULL) {
	    /*
	     * Ugly mechanism to save the raw entity value.
	     */
	    xmlEntityPtr cur = NULL;

	    if (isParameter) {
	        if ((ctxt->sax != NULL) &&
		    (ctxt->sax->getParameterEntity != NULL))
		    cur = ctxt->sax->getParameterEntity(ctxt->userData, name);
	    } else {
	        if ((ctxt->sax != NULL) &&
		    (ctxt->sax->getEntity != NULL))
		    cur = ctxt->sax->getEntity(ctxt->userData, name);
		if ((cur == NULL) && (ctxt->userData==ctxt)) {
		    cur = xmlSAX2GetEntity(ctxt, name);
		}
	    }
            if (cur != NULL) {
	        if (cur->orig != NULL)
		    xmlFree(orig);
		else
		    cur->orig = orig;
	    } else
		xmlFree(orig);
	}
	if (value != NULL) xmlFree(value);
	if (URI != NULL) xmlFree(URI);
	if (literal != NULL) xmlFree(literal);
    }
}

/**
 * xmlParseDefaultDecl:
 * @ctxt:  an XML parser context
 * @value:  Receive a possible fixed default value for the attribute
 *
 * Parse an attribute default declaration
 *
 * [60] DefaultDecl ::= '#REQUIRED' | '#IMPLIED' | (('#FIXED' S)? AttValue)
 *
 * [ VC: Required Attribute ]
 * if the default declaration is the keyword #REQUIRED, then the
 * attribute must be specified for all elements of the type in the
 * attribute-list declaration.
 *
 * [ VC: Attribute Default Legal ]
 * The declared default value must meet the lexical constraints of
 * the declared attribute type c.f. xmlValidateAttributeDecl()
 *
 * [ VC: Fixed Attribute Default ]
 * if an attribute has a default value declared with the #FIXED
 * keyword, instances of that attribute must match the default value. 
 *
 * [ WFC: No < in Attribute Values ]
 * handled in xmlParseAttValue()
 *
 * returns: XML_ATTRIBUTE_NONE, XML_ATTRIBUTE_REQUIRED, XML_ATTRIBUTE_IMPLIED
 *          or XML_ATTRIBUTE_FIXED. 
 */

int
xmlParseDefaultDecl(xmlParserCtxtPtr ctxt, xmlChar **value) {
    int val;
    xmlChar *ret;

    *value = NULL;
    if (CMP9(CUR_PTR, '#', 'R', 'E', 'Q', 'U', 'I', 'R', 'E', 'D')) {
	SKIP(9);
	return(XML_ATTRIBUTE_REQUIRED);
    }
    if (CMP8(CUR_PTR, '#', 'I', 'M', 'P', 'L', 'I', 'E', 'D')) {
	SKIP(8);
	return(XML_ATTRIBUTE_IMPLIED);
    }
    val = XML_ATTRIBUTE_NONE;
    if (CMP6(CUR_PTR, '#', 'F', 'I', 'X', 'E', 'D')) {
	SKIP(6);
	val = XML_ATTRIBUTE_FIXED;
	if (!IS_BLANK_CH(CUR)) {
	    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
			   "Space required after '#FIXED'\n");
	}
	SKIP_BLANKS;
    }
    ret = xmlParseAttValue(ctxt);
    ctxt->instate = XML_PARSER_DTD;
    if (ret == NULL) {
	xmlFatalErrMsg(ctxt, (xmlParserErrors)ctxt->errNo,
		       "Attribute default value declaration error\n");
    } else
        *value = ret;
    return(val);
}

/**
 * xmlParseNotationType:
 * @ctxt:  an XML parser context
 *
 * parse an Notation attribute type.
 *
 * Note: the leading 'NOTATION' S part has already being parsed...
 *
 * [58] NotationType ::= 'NOTATION' S '(' S? Name (S? '|' S? Name)* S? ')'
 *
 * [ VC: Notation Attributes ]
 * Values of this type must match one of the notation names included
 * in the declaration; all notation names in the declaration must be declared. 
 *
 * Returns: the notation attribute tree built while parsing
 */

xmlEnumerationPtr
xmlParseNotationType(xmlParserCtxtPtr ctxt) {
    const xmlChar *name;
    xmlEnumerationPtr ret = NULL, last = NULL, cur;

    if (RAW != '(') {
	xmlFatalErr(ctxt, XML_ERR_NOTATION_NOT_STARTED, NULL);
	return(NULL);
    }
    SHRINK;
    do {
        NEXT;
	SKIP_BLANKS;
        name = xmlParseName(ctxt);
	if (name == NULL) {
	    xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED,
			   "Name expected in NOTATION declaration\n");
	    return(ret);
	}
	cur = xmlCreateEnumeration(name);
	if (cur == NULL) return(ret);
	if (last == NULL) ret = last = cur;
	else {
	    last->next = cur;
	    last = cur;
	}
	SKIP_BLANKS;
    } while (RAW == '|');
    if (RAW != ')') {
	xmlFatalErr(ctxt, XML_ERR_NOTATION_NOT_FINISHED, NULL);
	if ((last != NULL) && (last != ret))
	    xmlFreeEnumeration(last);
	return(ret);
    }
    NEXT;
    return(ret);
}

/**
 * xmlParseEnumerationType:
 * @ctxt:  an XML parser context
 *
 * parse an Enumeration attribute type.
 *
 * [59] Enumeration ::= '(' S? Nmtoken (S? '|' S? Nmtoken)* S? ')'
 *
 * [ VC: Enumeration ]
 * Values of this type must match one of the Nmtoken tokens in
 * the declaration
 *
 * Returns: the enumeration attribute tree built while parsing
 */

xmlEnumerationPtr
xmlParseEnumerationType(xmlParserCtxtPtr ctxt) {
    xmlChar *name;
    xmlEnumerationPtr ret = NULL, last = NULL, cur;

    if (RAW != '(') {
	xmlFatalErr(ctxt, XML_ERR_ATTLIST_NOT_STARTED, NULL);
	return(NULL);
    }
    SHRINK;
    do {
        NEXT;
	SKIP_BLANKS;
        name = xmlParseNmtoken(ctxt);
	if (name == NULL) {
	    xmlFatalErr(ctxt, XML_ERR_NMTOKEN_REQUIRED, NULL);
	    return(ret);
	}
	cur = xmlCreateEnumeration(name);
	xmlFree(name);
	if (cur == NULL) return(ret);
	if (last == NULL) ret = last = cur;
	else {
	    last->next = cur;
	    last = cur;
	}
	SKIP_BLANKS;
    } while (RAW == '|');
    if (RAW != ')') {
	xmlFatalErr(ctxt, XML_ERR_ATTLIST_NOT_FINISHED, NULL);
	return(ret);
    }
    NEXT;
    return(ret);
}

/**
 * xmlParseEnumeratedType:
 * @ctxt:  an XML parser context
 * @tree:  the enumeration tree built while parsing
 *
 * parse an Enumerated attribute type.
 *
 * [57] EnumeratedType ::= NotationType | Enumeration
 *
 * [58] NotationType ::= 'NOTATION' S '(' S? Name (S? '|' S? Name)* S? ')'
 *
 *
 * Returns: XML_ATTRIBUTE_ENUMERATION or XML_ATTRIBUTE_NOTATION
 */

int
xmlParseEnumeratedType(xmlParserCtxtPtr ctxt, xmlEnumerationPtr *tree) {
    if (CMP8(CUR_PTR, 'N', 'O', 'T', 'A', 'T', 'I', 'O', 'N')) {
	SKIP(8);
	if (!IS_BLANK_CH(CUR)) {
	    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
			   "Space required after 'NOTATION'\n");
	    return(0);
	}
        SKIP_BLANKS;
	*tree = xmlParseNotationType(ctxt);
	if (*tree == NULL) return(0);
	return(XML_ATTRIBUTE_NOTATION);
    }
    *tree = xmlParseEnumerationType(ctxt);
    if (*tree == NULL) return(0);
    return(XML_ATTRIBUTE_ENUMERATION);
}

/**
 * xmlParseAttributeType:
 * @ctxt:  an XML parser context
 * @tree:  the enumeration tree built while parsing
 *
 * parse the Attribute list def for an element
 *
 * [54] AttType ::= StringType | TokenizedType | EnumeratedType
 *
 * [55] StringType ::= 'CDATA'
 *
 * [56] TokenizedType ::= 'ID' | 'IDREF' | 'IDREFS' | 'ENTITY' |
 *                        'ENTITIES' | 'NMTOKEN' | 'NMTOKENS'
 *
 * Validity constraints for attribute values syntax are checked in
 * xmlValidateAttributeValue()
 *
 * [ VC: ID ]
 * Values of type ID must match the Name production. A name must not
 * appear more than once in an XML document as a value of this type;
 * i.e., ID values must uniquely identify the elements which bear them.
 *
 * [ VC: One ID per Element Type ]
 * No element type may have more than one ID attribute specified.
 *
 * [ VC: ID Attribute Default ]
 * An ID attribute must have a declared default of #IMPLIED or #REQUIRED.
 *
 * [ VC: IDREF ]
 * Values of type IDREF must match the Name production, and values
 * of type IDREFS must match Names; each IDREF Name must match the value
 * of an ID attribute on some element in the XML document; i.e. IDREF
 * values must match the value of some ID attribute.
 *
 * [ VC: Entity Name ]
 * Values of type ENTITY must match the Name production, values
 * of type ENTITIES must match Names; each Entity Name must match the
 * name of an unparsed entity declared in the DTD.  
 *
 * [ VC: Name Token ]
 * Values of type NMTOKEN must match the Nmtoken production; values
 * of type NMTOKENS must match Nmtokens. 
 *
 * Returns the attribute type
 */
int 
xmlParseAttributeType(xmlParserCtxtPtr ctxt, xmlEnumerationPtr *tree) {
    SHRINK;
    if (CMP5(CUR_PTR, 'C', 'D', 'A', 'T', 'A')) {
	SKIP(5);
	return(XML_ATTRIBUTE_CDATA);
     } else if (CMP6(CUR_PTR, 'I', 'D', 'R', 'E', 'F', 'S')) {
	SKIP(6);
	return(XML_ATTRIBUTE_IDREFS);
     } else if (CMP5(CUR_PTR, 'I', 'D', 'R', 'E', 'F')) {
	SKIP(5);
	return(XML_ATTRIBUTE_IDREF);
     } else if ((RAW == 'I') && (NXT(1) == 'D')) {
        SKIP(2);
	return(XML_ATTRIBUTE_ID);
     } else if (CMP6(CUR_PTR, 'E', 'N', 'T', 'I', 'T', 'Y')) {
	SKIP(6);
	return(XML_ATTRIBUTE_ENTITY);
     } else if (CMP8(CUR_PTR, 'E', 'N', 'T', 'I', 'T', 'I', 'E', 'S')) {
	SKIP(8);
	return(XML_ATTRIBUTE_ENTITIES);
     } else if (CMP8(CUR_PTR, 'N', 'M', 'T', 'O', 'K', 'E', 'N', 'S')) {
	SKIP(8);
	return(XML_ATTRIBUTE_NMTOKENS);
     } else if (CMP7(CUR_PTR, 'N', 'M', 'T', 'O', 'K', 'E', 'N')) {
	SKIP(7);
	return(XML_ATTRIBUTE_NMTOKEN);
     }
     return(xmlParseEnumeratedType(ctxt, tree));
}

/**
 * xmlParseAttributeListDecl:
 * @ctxt:  an XML parser context
 *
 * : parse the Attribute list def for an element
 *
 * [52] AttlistDecl ::= '<!ATTLIST' S Name AttDef* S? '>'
 *
 * [53] AttDef ::= S Name S AttType S DefaultDecl
 *
 */
void
xmlParseAttributeListDecl(xmlParserCtxtPtr ctxt) {
    const xmlChar *elemName;
    const xmlChar *attrName;
    xmlEnumerationPtr tree;

    if (CMP9(CUR_PTR, '<', '!', 'A', 'T', 'T', 'L', 'I', 'S', 'T')) {
	xmlParserInputPtr input = ctxt->input;

	SKIP(9);
	if (!IS_BLANK_CH(CUR)) {
	    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
		                 "Space required after '<!ATTLIST'\n");
	}
        SKIP_BLANKS;
        elemName = xmlParseName(ctxt);
	if (elemName == NULL) {
	    xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED,
			   "ATTLIST: no name for Element\n");
	    return;
	}
	SKIP_BLANKS;
	GROW;
	while (RAW != '>') {
	    const xmlChar *check = CUR_PTR;
	    int type;
	    int def;
	    xmlChar *defaultValue = NULL;

	    GROW;
            tree = NULL;
	    attrName = xmlParseName(ctxt);
	    if (attrName == NULL) {
		xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED,
			       "ATTLIST: no name for Attribute\n");
		break;
	    }
	    GROW;
	    if (!IS_BLANK_CH(CUR)) {
		xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
		        "Space required after the attribute name\n");
                if (defaultValue != NULL)
		    xmlFree(defaultValue);
		break;
	    }
	    SKIP_BLANKS;

	    type = xmlParseAttributeType(ctxt, &tree);
	    if (type <= 0) {
                if (defaultValue != NULL)
		    xmlFree(defaultValue);
	        break;
	    }

	    GROW;
	    if (!IS_BLANK_CH(CUR)) {
		xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
			       "Space required after the attribute type\n");
                if (defaultValue != NULL)
		    xmlFree(defaultValue);
	        if (tree != NULL)
		    xmlFreeEnumeration(tree);
		break;
	    }
	    SKIP_BLANKS;

	    def = xmlParseDefaultDecl(ctxt, &defaultValue);
	    if (def <= 0) {
                if (defaultValue != NULL)
		    xmlFree(defaultValue);
	        if (tree != NULL)
		    xmlFreeEnumeration(tree);
	        break;
	    }

	    GROW;
            if (RAW != '>') {
		if (!IS_BLANK_CH(CUR)) {
		    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
			"Space required after the attribute default value\n");
		    if (defaultValue != NULL)
			xmlFree(defaultValue);
		    if (tree != NULL)
			xmlFreeEnumeration(tree);
		    break;
		}
		SKIP_BLANKS;
	    }
	    if (check == CUR_PTR) {
		xmlFatalErr(ctxt, XML_ERR_INTERNAL_ERROR,
		            "in xmlParseAttributeListDecl\n");
		if (defaultValue != NULL)
		    xmlFree(defaultValue);
	        if (tree != NULL)
		    xmlFreeEnumeration(tree);
		break;
	    }
	    if ((ctxt->sax != NULL) && (!ctxt->disableSAX) &&
		(ctxt->sax->attributeDecl != NULL))
		ctxt->sax->attributeDecl(ctxt->userData, elemName, attrName,
	                        type, def, defaultValue, tree);
	    else if (tree != NULL)
		xmlFreeEnumeration(tree);

	    if ((ctxt->sax2) && (defaultValue != NULL) &&
	        (def != XML_ATTRIBUTE_IMPLIED) && 
		(def != XML_ATTRIBUTE_REQUIRED)) {
		xmlAddDefAttrs(ctxt, elemName, attrName, defaultValue);
	    }
	    if ((ctxt->sax2) && (type != XML_ATTRIBUTE_CDATA)) {
		xmlAddSpecialAttr(ctxt, elemName, attrName, type);
	    }
	    if (defaultValue != NULL)
	        xmlFree(defaultValue);
	    GROW;
	}
	if (RAW == '>') {
	    if (input != ctxt->input) {
		xmlFatalErrMsg(ctxt, XML_ERR_ENTITY_BOUNDARY,
    "Attribute list declaration doesn't start and stop in the same entity\n");
	    }
	    NEXT;
	}
    }
}

/**
 * xmlParseElementMixedContentDecl:
 * @ctxt:  an XML parser context
 * @inputchk:  the input used for the current entity, needed for boundary checks
 *
 * parse the declaration for a Mixed Element content
 * The leading '(' and spaces have been skipped in xmlParseElementContentDecl
 * 
 * [51] Mixed ::= '(' S? '#PCDATA' (S? '|' S? Name)* S? ')*' |
 *                '(' S? '#PCDATA' S? ')'
 *
 * [ VC: Proper Group/PE Nesting ] applies to [51] too (see [49])
 *
 * [ VC: No Duplicate Types ]
 * The same name must not appear more than once in a single
 * mixed-content declaration. 
 *
 * returns: the list of the xmlElementContentPtr describing the element choices
 */
xmlElementContentPtr
xmlParseElementMixedContentDecl(xmlParserCtxtPtr ctxt, int inputchk) {
    xmlElementContentPtr ret = NULL, cur = NULL, n;
    const xmlChar *elem = NULL;

    GROW;
    if (CMP7(CUR_PTR, '#', 'P', 'C', 'D', 'A', 'T', 'A')) {
	SKIP(7);
	SKIP_BLANKS;
	SHRINK;
	if (RAW == ')') {
	    if ((ctxt->validate) && (ctxt->input->id != inputchk)) {
		xmlValidityError(ctxt, XML_ERR_ENTITY_BOUNDARY,
"Element content declaration doesn't start and stop in the same entity\n",
                                 NULL);
	    }
	    NEXT;
	    ret = xmlNewElementContent(NULL, XML_ELEMENT_CONTENT_PCDATA);
	    if (RAW == '*') {
		ret->ocur = XML_ELEMENT_CONTENT_MULT;
		NEXT;
	    }
	    return(ret);
	}
	if ((RAW == '(') || (RAW == '|')) {
	    ret = cur = xmlNewElementContent(NULL, XML_ELEMENT_CONTENT_PCDATA);
	    if (ret == NULL) return(NULL);
	}
	while (RAW == '|') {
	    NEXT;
	    if (elem == NULL) {
	        ret = xmlNewElementContent(NULL, XML_ELEMENT_CONTENT_OR);
		if (ret == NULL) return(NULL);
		ret->c1 = cur;
		if (cur != NULL)
		    cur->parent = ret;
		cur = ret;
	    } else {
	        n = xmlNewElementContent(NULL, XML_ELEMENT_CONTENT_OR);
		if (n == NULL) return(NULL);
		n->c1 = xmlNewElementContent(elem, XML_ELEMENT_CONTENT_ELEMENT);
		if (n->c1 != NULL)
		    n->c1->parent = n;
	        cur->c2 = n;
		if (n != NULL)
		    n->parent = cur;
		cur = n;
	    }
	    SKIP_BLANKS;
	    elem = xmlParseName(ctxt);
	    if (elem == NULL) {
		xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED,
			"xmlParseElementMixedContentDecl : Name expected\n");
		xmlFreeElementContent(cur);
		return(NULL);
	    }
	    SKIP_BLANKS;
	    GROW;
	}
	if ((RAW == ')') && (NXT(1) == '*')) {
	    if (elem != NULL) {
		cur->c2 = xmlNewElementContent(elem,
		                               XML_ELEMENT_CONTENT_ELEMENT);
		if (cur->c2 != NULL)
		    cur->c2->parent = cur;
            }
	    ret->ocur = XML_ELEMENT_CONTENT_MULT;
	    if ((ctxt->validate) && (ctxt->input->id != inputchk)) {
		xmlValidityError(ctxt, XML_ERR_ENTITY_BOUNDARY,
"Element content declaration doesn't start and stop in the same entity\n",
				 NULL);
	    }
	    SKIP(2);
	} else {
	    xmlFreeElementContent(ret);
	    xmlFatalErr(ctxt, XML_ERR_MIXED_NOT_STARTED, NULL);
	    return(NULL);
	}

    } else {
	xmlFatalErr(ctxt, XML_ERR_PCDATA_REQUIRED, NULL);
    }
    return(ret);
}

/**
 * xmlParseElementChildrenContentDecl:
 * @ctxt:  an XML parser context
 * @inputchk:  the input used for the current entity, needed for boundary checks
 *
 * parse the declaration for a Mixed Element content
 * The leading '(' and spaces have been skipped in xmlParseElementContentDecl
 * 
 *
 * [47] children ::= (choice | seq) ('?' | '*' | '+')?
 *
 * [48] cp ::= (Name | choice | seq) ('?' | '*' | '+')?
 *
 * [49] choice ::= '(' S? cp ( S? '|' S? cp )* S? ')'
 *
 * [50] seq ::= '(' S? cp ( S? ',' S? cp )* S? ')'
 *
 * [ VC: Proper Group/PE Nesting ] applies to [49] and [50]
 * TODO Parameter-entity replacement text must be properly nested
 *	with parenthesized groups. That is to say, if either of the
 *	opening or closing parentheses in a choice, seq, or Mixed
 *	construct is contained in the replacement text for a parameter
 *	entity, both must be contained in the same replacement text. For
 *	interoperability, if a parameter-entity reference appears in a
 *	choice, seq, or Mixed construct, its replacement text should not
 *	be empty, and neither the first nor last non-blank character of
 *	the replacement text should be a connector (| or ,).
 *
 * Returns the tree of xmlElementContentPtr describing the element 
 *          hierarchy.
 */
xmlElementContentPtr
xmlParseElementChildrenContentDecl (xmlParserCtxtPtr ctxt, int inputchk) {
    xmlElementContentPtr ret = NULL, cur = NULL, last = NULL, op = NULL;
    const xmlChar *elem;
    xmlChar type = 0;

    SKIP_BLANKS;
    GROW;
    if (RAW == '(') {
	int inputid = ctxt->input->id;

        /* Recurse on first child */
	NEXT;
	SKIP_BLANKS;
        cur = ret = xmlParseElementChildrenContentDecl(ctxt, inputid);
	SKIP_BLANKS;
	GROW;
    } else {
	elem = xmlParseName(ctxt);
	if (elem == NULL) {
	    xmlFatalErr(ctxt, XML_ERR_ELEMCONTENT_NOT_STARTED, NULL);
	    return(NULL);
	}
        cur = ret = xmlNewElementContent(elem, XML_ELEMENT_CONTENT_ELEMENT);
	if (cur == NULL) {
	    xmlErrMemory(ctxt, NULL);
	    return(NULL);
	}
	GROW;
	if (RAW == '?') {
	    cur->ocur = XML_ELEMENT_CONTENT_OPT;
	    NEXT;
	} else if (RAW == '*') {
	    cur->ocur = XML_ELEMENT_CONTENT_MULT;
	    NEXT;
	} else if (RAW == '+') {
	    cur->ocur = XML_ELEMENT_CONTENT_PLUS;
	    NEXT;
	} else {
	    cur->ocur = XML_ELEMENT_CONTENT_ONCE;
	}
	GROW;
    }
    SKIP_BLANKS;
    SHRINK;
    while (RAW != ')') {
        /*
	 * Each loop we parse one separator and one element.
	 */
        if (RAW == ',') {
	    if (type == 0) type = CUR;

	    /*
	     * Detect "Name | Name , Name" error
	     */
	    else if (type != CUR) {
		xmlFatalErrMsgInt(ctxt, XML_ERR_SEPARATOR_REQUIRED,
		    "xmlParseElementChildrenContentDecl : '%c' expected\n",
		                  type);
		if ((last != NULL) && (last != ret))
		    xmlFreeElementContent(last);
		if (ret != NULL)
		    xmlFreeElementContent(ret);
		return(NULL);
	    }
	    NEXT;

	    op = xmlNewElementContent(NULL, XML_ELEMENT_CONTENT_SEQ);
	    if (op == NULL) {
		if ((last != NULL) && (last != ret))
		    xmlFreeElementContent(last);
	        xmlFreeElementContent(ret);
		return(NULL);
	    }
	    if (last == NULL) {
		op->c1 = ret;
		if (ret != NULL)
		    ret->parent = op;
		ret = cur = op;
	    } else {
	        cur->c2 = op;
		if (op != NULL)
		    op->parent = cur;
		op->c1 = last;
		if (last != NULL)
		    last->parent = op;
		cur =op;
		last = NULL;
	    }
	} else if (RAW == '|') {
	    if (type == 0) type = CUR;

	    /*
	     * Detect "Name , Name | Name" error
	     */
	    else if (type != CUR) {
		xmlFatalErrMsgInt(ctxt, XML_ERR_SEPARATOR_REQUIRED,
		    "xmlParseElementChildrenContentDecl : '%c' expected\n",
				  type);
		if ((last != NULL) && (last != ret))
		    xmlFreeElementContent(last);
		if (ret != NULL)
		    xmlFreeElementContent(ret);
		return(NULL);
	    }
	    NEXT;

	    op = xmlNewElementContent(NULL, XML_ELEMENT_CONTENT_OR);
	    if (op == NULL) {
		if ((last != NULL) && (last != ret))
		    xmlFreeElementContent(last);
		if (ret != NULL)
		    xmlFreeElementContent(ret);
		return(NULL);
	    }
	    if (last == NULL) {
		op->c1 = ret;
		if (ret != NULL)
		    ret->parent = op;
		ret = cur = op;
	    } else {
	        cur->c2 = op;
		if (op != NULL)
		    op->parent = cur;
		op->c1 = last;
		if (last != NULL)
		    last->parent = op;
		cur =op;
		last = NULL;
	    }
	} else {
	    xmlFatalErr(ctxt, XML_ERR_ELEMCONTENT_NOT_FINISHED, NULL);
	    if (ret != NULL)
		xmlFreeElementContent(ret);
	    return(NULL);
	}
	GROW;
	SKIP_BLANKS;
	GROW;
	if (RAW == '(') {
	    int inputid = ctxt->input->id;
	    /* Recurse on second child */
	    NEXT;
	    SKIP_BLANKS;
	    last = xmlParseElementChildrenContentDecl(ctxt, inputid);
	    SKIP_BLANKS;
	} else {
	    elem = xmlParseName(ctxt);
	    if (elem == NULL) {
		xmlFatalErr(ctxt, XML_ERR_ELEMCONTENT_NOT_STARTED, NULL);
		if (ret != NULL)
		    xmlFreeElementContent(ret);
		return(NULL);
	    }
	    last = xmlNewElementContent(elem, XML_ELEMENT_CONTENT_ELEMENT);
	    if (RAW == '?') {
		last->ocur = XML_ELEMENT_CONTENT_OPT;
		NEXT;
	    } else if (RAW == '*') {
		last->ocur = XML_ELEMENT_CONTENT_MULT;
		NEXT;
	    } else if (RAW == '+') {
		last->ocur = XML_ELEMENT_CONTENT_PLUS;
		NEXT;
	    } else {
		last->ocur = XML_ELEMENT_CONTENT_ONCE;
	    }
	}
	SKIP_BLANKS;
	GROW;
    }
    if ((cur != NULL) && (last != NULL)) {
        cur->c2 = last;
	if (last != NULL)
	    last->parent = cur;
    }
    if ((ctxt->validate) && (ctxt->input->id != inputchk)) {
	xmlValidityError(ctxt, XML_ERR_ENTITY_BOUNDARY,
"Element content declaration doesn't start and stop in the same entity\n",
			 NULL);
    }
    NEXT;
    if (RAW == '?') {
	if (ret != NULL)
	    ret->ocur = XML_ELEMENT_CONTENT_OPT;
	NEXT;
    } else if (RAW == '*') {
	if (ret != NULL) {
	    ret->ocur = XML_ELEMENT_CONTENT_MULT;
	    cur = ret;
	    /*
	     * Some normalization:
	     * (a | b* | c?)* == (a | b | c)*
	     */
	    while (cur->type == XML_ELEMENT_CONTENT_OR) {
		if ((cur->c1 != NULL) &&
	            ((cur->c1->ocur == XML_ELEMENT_CONTENT_OPT) ||
		     (cur->c1->ocur == XML_ELEMENT_CONTENT_MULT)))
		    cur->c1->ocur = XML_ELEMENT_CONTENT_ONCE;
		if ((cur->c2 != NULL) &&
	            ((cur->c2->ocur == XML_ELEMENT_CONTENT_OPT) ||
		     (cur->c2->ocur == XML_ELEMENT_CONTENT_MULT)))
		    cur->c2->ocur = XML_ELEMENT_CONTENT_ONCE;
		cur = cur->c2;
	    }
	}
	NEXT;
    } else if (RAW == '+') {
	if (ret != NULL) {
	    int found = 0;

	    ret->ocur = XML_ELEMENT_CONTENT_PLUS;
	    /*
	     * Some normalization:
	     * (a | b*)+ == (a | b)*
	     * (a | b?)+ == (a | b)*
	     */
	    while (cur->type == XML_ELEMENT_CONTENT_OR) {
		if ((cur->c1 != NULL) &&
	            ((cur->c1->ocur == XML_ELEMENT_CONTENT_OPT) ||
		     (cur->c1->ocur == XML_ELEMENT_CONTENT_MULT))) {
		    cur->c1->ocur = XML_ELEMENT_CONTENT_ONCE;
		    found = 1;
		}
		if ((cur->c2 != NULL) &&
	            ((cur->c2->ocur == XML_ELEMENT_CONTENT_OPT) ||
		     (cur->c2->ocur == XML_ELEMENT_CONTENT_MULT))) {
		    cur->c2->ocur = XML_ELEMENT_CONTENT_ONCE;
		    found = 1;
		}
		cur = cur->c2;
	    }
	    if (found)
		ret->ocur = XML_ELEMENT_CONTENT_MULT;
	}
	NEXT;
    }
    return(ret);
}

/**
 * xmlParseElementContentDecl:
 * @ctxt:  an XML parser context
 * @name:  the name of the element being defined.
 * @result:  the Element Content pointer will be stored here if any
 *
 * parse the declaration for an Element content either Mixed or Children,
 * the cases EMPTY and ANY are handled directly in xmlParseElementDecl
 * 
 * [46] contentspec ::= 'EMPTY' | 'ANY' | Mixed | children
 *
 * returns: the type of element content XML_ELEMENT_TYPE_xxx
 */

int
xmlParseElementContentDecl(xmlParserCtxtPtr ctxt, const xmlChar *name,
                           xmlElementContentPtr *result) {

    xmlElementContentPtr tree = NULL;
    int inputid = ctxt->input->id;
    int res;

    *result = NULL;

    if (RAW != '(') {
	xmlFatalErrMsgStr(ctxt, XML_ERR_ELEMCONTENT_NOT_STARTED,
		"xmlParseElementContentDecl : %s '(' expected\n", name);
	return(-1);
    }
    NEXT;
    GROW;
    SKIP_BLANKS;
    if (CMP7(CUR_PTR, '#', 'P', 'C', 'D', 'A', 'T', 'A')) {
        tree = xmlParseElementMixedContentDecl(ctxt, inputid);
	res = XML_ELEMENT_TYPE_MIXED;
    } else {
        tree = xmlParseElementChildrenContentDecl(ctxt, inputid);
	res = XML_ELEMENT_TYPE_ELEMENT;
    }
    SKIP_BLANKS;
    *result = tree;
    return(res);
}

/**
 * xmlParseElementDecl:
 * @ctxt:  an XML parser context
 *
 * parse an Element declaration.
 *
 * [45] elementdecl ::= '<!ELEMENT' S Name S contentspec S? '>'
 *
 * [ VC: Unique Element Type Declaration ]
 * No element type may be declared more than once
 *
 * Returns the type of the element, or -1 in case of error
 */
int
xmlParseElementDecl(xmlParserCtxtPtr ctxt) {
    const xmlChar *name;
    int ret = -1;
    xmlElementContentPtr content  = NULL;

    GROW;
    if (CMP9(CUR_PTR, '<', '!', 'E', 'L', 'E', 'M', 'E', 'N', 'T')) {
	xmlParserInputPtr input = ctxt->input;

	SKIP(9);
	if (!IS_BLANK_CH(CUR)) {
	    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
		           "Space required after 'ELEMENT'\n");
	}
        SKIP_BLANKS;
        name = xmlParseName(ctxt);
	if (name == NULL) {
	    xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED,
			   "xmlParseElementDecl: no name for Element\n");
	    return(-1);
	}
	while ((RAW == 0) && (ctxt->inputNr > 1))
	    xmlPopInput(ctxt);
	if (!IS_BLANK_CH(CUR)) {
	    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
			   "Space required after the element name\n");
	}
        SKIP_BLANKS;
	if (CMP5(CUR_PTR, 'E', 'M', 'P', 'T', 'Y')) {
	    SKIP(5);
	    /*
	     * Element must always be empty.
	     */
	    ret = XML_ELEMENT_TYPE_EMPTY;
	} else if ((RAW == 'A') && (NXT(1) == 'N') &&
	           (NXT(2) == 'Y')) {
	    SKIP(3);
	    /*
	     * Element is a generic container.
	     */
	    ret = XML_ELEMENT_TYPE_ANY;
	} else if (RAW == '(') {
	    ret = xmlParseElementContentDecl(ctxt, name, &content);
	} else {
	    /*
	     * [ WFC: PEs in Internal Subset ] error handling.
	     */
	    if ((RAW == '%') && (ctxt->external == 0) &&
	        (ctxt->inputNr == 1)) {
		xmlFatalErrMsg(ctxt, XML_ERR_PEREF_IN_INT_SUBSET,
	  "PEReference: forbidden within markup decl in internal subset\n");
	    } else {
		xmlFatalErrMsg(ctxt, XML_ERR_ELEMCONTENT_NOT_STARTED,
		      "xmlParseElementDecl: 'EMPTY', 'ANY' or '(' expected\n");
            }
	    return(-1);
	}

	SKIP_BLANKS;
	/*
	 * Pop-up of finished entities.
	 */
	while ((RAW == 0) && (ctxt->inputNr > 1))
	    xmlPopInput(ctxt);
	SKIP_BLANKS;

	if (RAW != '>') {
	    xmlFatalErr(ctxt, XML_ERR_GT_REQUIRED, NULL);
	} else {
	    if (input != ctxt->input) {
		xmlFatalErrMsg(ctxt, XML_ERR_ENTITY_BOUNDARY,
    "Element declaration doesn't start and stop in the same entity\n");
	    }
		
	    NEXT;
	    if ((ctxt->sax != NULL) && (!ctxt->disableSAX) &&
		(ctxt->sax->elementDecl != NULL))
	        ctxt->sax->elementDecl(ctxt->userData, name, ret,
		                       content);
	}
	if (content != NULL) {
	    xmlFreeElementContent(content);
	}
    }
    return(ret);
}

/**
 * xmlParseConditionalSections
 * @ctxt:  an XML parser context
 *
 * [61] conditionalSect ::= includeSect | ignoreSect 
 * [62] includeSect ::= '<![' S? 'INCLUDE' S? '[' extSubsetDecl ']]>' 
 * [63] ignoreSect ::= '<![' S? 'IGNORE' S? '[' ignoreSectContents* ']]>'
 * [64] ignoreSectContents ::= Ignore ('<![' ignoreSectContents ']]>' Ignore)*
 * [65] Ignore ::= Char* - (Char* ('<![' | ']]>') Char*)
 */

static void
xmlParseConditionalSections(xmlParserCtxtPtr ctxt) {
    SKIP(3);
    SKIP_BLANKS;
    if (CMP7(CUR_PTR, 'I', 'N', 'C', 'L', 'U', 'D', 'E')) {
	SKIP(7);
	SKIP_BLANKS;
	if (RAW != '[') {
	    xmlFatalErr(ctxt, XML_ERR_CONDSEC_INVALID, NULL);
	} else {
	    NEXT;
	}
	if (xmlParserDebugEntities) {
	    if ((ctxt->input != NULL) && (ctxt->input->filename))
		xmlGenericError(xmlGenericErrorContext,
			"%s(%d): ", ctxt->input->filename,
			ctxt->input->line);
	    xmlGenericError(xmlGenericErrorContext,
		    "Entering INCLUDE Conditional Section\n");
	}

	while ((RAW != 0) && ((RAW != ']') || (NXT(1) != ']') ||
	       (NXT(2) != '>'))) {
	    const xmlChar *check = CUR_PTR;
	    unsigned int cons = ctxt->input->consumed;

	    if ((RAW == '<') && (NXT(1) == '!') && (NXT(2) == '[')) {
		xmlParseConditionalSections(ctxt);
	    } else if (IS_BLANK_CH(CUR)) {
		NEXT;
	    } else if (RAW == '%') {
		xmlParsePEReference(ctxt);
	    } else
		xmlParseMarkupDecl(ctxt);

	    /*
	     * Pop-up of finished entities.
	     */
	    while ((RAW == 0) && (ctxt->inputNr > 1))
		xmlPopInput(ctxt);

	    if ((CUR_PTR == check) && (cons == ctxt->input->consumed)) {
		xmlFatalErr(ctxt, XML_ERR_EXT_SUBSET_NOT_FINISHED, NULL);
		break;
	    }
	}
	if (xmlParserDebugEntities) {
	    if ((ctxt->input != NULL) && (ctxt->input->filename))
		xmlGenericError(xmlGenericErrorContext,
			"%s(%d): ", ctxt->input->filename,
			ctxt->input->line);
	    xmlGenericError(xmlGenericErrorContext,
		    "Leaving INCLUDE Conditional Section\n");
	}

    } else if (CMP6(CUR_PTR, 'I', 'G', 'N', 'O', 'R', 'E')) {
	int state;
	xmlParserInputState instate;
	int depth = 0;

	SKIP(6);
	SKIP_BLANKS;
	if (RAW != '[') {
	    xmlFatalErr(ctxt, XML_ERR_CONDSEC_INVALID, NULL);
	} else {
	    NEXT;
	}
	if (xmlParserDebugEntities) {
	    if ((ctxt->input != NULL) && (ctxt->input->filename))
		xmlGenericError(xmlGenericErrorContext,
			"%s(%d): ", ctxt->input->filename,
			ctxt->input->line);
	    xmlGenericError(xmlGenericErrorContext,
		    "Entering IGNORE Conditional Section\n");
	}

	/*
	 * Parse up to the end of the conditional section
	 * But disable SAX event generating DTD building in the meantime
	 */
	state = ctxt->disableSAX;
	instate = ctxt->instate;
	if (ctxt->recovery == 0) ctxt->disableSAX = 1;
	ctxt->instate = XML_PARSER_IGNORE;

	while ((depth >= 0) && (RAW != 0)) {
	  if ((RAW == '<') && (NXT(1) == '!') && (NXT(2) == '[')) {
	    depth++;
	    SKIP(3);
	    continue;
	  }
	  if ((RAW == ']') && (NXT(1) == ']') && (NXT(2) == '>')) {
	    if (--depth >= 0) SKIP(3);
	    continue;
	  }
	  NEXT;
	  continue;
	}

	ctxt->disableSAX = state;
	ctxt->instate = instate;

	if (xmlParserDebugEntities) {
	    if ((ctxt->input != NULL) && (ctxt->input->filename))
		xmlGenericError(xmlGenericErrorContext,
			"%s(%d): ", ctxt->input->filename,
			ctxt->input->line);
	    xmlGenericError(xmlGenericErrorContext,
		    "Leaving IGNORE Conditional Section\n");
	}

    } else {
	xmlFatalErr(ctxt, XML_ERR_CONDSEC_INVALID_KEYWORD, NULL);
    }

    if (RAW == 0)
        SHRINK;

    if (RAW == 0) {
	xmlFatalErr(ctxt, XML_ERR_CONDSEC_NOT_FINISHED, NULL);
    } else {
        SKIP(3);
    }
}

/**
 * xmlParseMarkupDecl:
 * @ctxt:  an XML parser context
 * 
 * parse Markup declarations
 *
 * [29] markupdecl ::= elementdecl | AttlistDecl | EntityDecl |
 *                     NotationDecl | PI | Comment
 *
 * [ VC: Proper Declaration/PE Nesting ]
 * Parameter-entity replacement text must be properly nested with
 * markup declarations. That is to say, if either the first character
 * or the last character of a markup declaration (markupdecl above) is
 * contained in the replacement text for a parameter-entity reference,
 * both must be contained in the same replacement text.
 *
 * [ WFC: PEs in Internal Subset ]
 * In the internal DTD subset, parameter-entity references can occur
 * only where markup declarations can occur, not within markup declarations.
 * (This does not apply to references that occur in external parameter
 * entities or to the external subset.) 
 */
void
xmlParseMarkupDecl(xmlParserCtxtPtr ctxt) {
    GROW;
    xmlParseElementDecl(ctxt);
    xmlParseAttributeListDecl(ctxt);
    xmlParseEntityDecl(ctxt);
    xmlParseNotationDecl(ctxt);
    xmlParsePI(ctxt);
    xmlParseComment(ctxt);
    /*
     * This is only for internal subset. On external entities,
     * the replacement is done before parsing stage
     */
    if ((ctxt->external == 0) && (ctxt->inputNr == 1))
	xmlParsePEReference(ctxt);

    /*
     * Conditional sections are allowed from entities included
     * by PE References in the internal subset.
     */
    if ((ctxt->external == 0) && (ctxt->inputNr > 1)) {
        if ((RAW == '<') && (NXT(1) == '!') && (NXT(2) == '[')) {
	    xmlParseConditionalSections(ctxt);
	}
    }

    ctxt->instate = XML_PARSER_DTD;
}

/**
 * xmlParseTextDecl:
 * @ctxt:  an XML parser context
 * 
 * parse an XML declaration header for external entities
 *
 * [77] TextDecl ::= '<?xml' VersionInfo? EncodingDecl S? '?>'
 *
 * Question: Seems that EncodingDecl is mandatory ? Is that a typo ?
 */

void
xmlParseTextDecl(xmlParserCtxtPtr ctxt) {
    xmlChar *version;

    /*
     * We know that '<?xml' is here.
     */
    if ((CMP5(CUR_PTR, '<', '?', 'x', 'm', 'l')) && (IS_BLANK_CH(NXT(5)))) {
	SKIP(5);
    } else {
	xmlFatalErr(ctxt, XML_ERR_XMLDECL_NOT_STARTED, NULL);
	return;
    }

    if (!IS_BLANK_CH(CUR)) {
	xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
		       "Space needed after '<?xml'\n");
    }
    SKIP_BLANKS;

    /*
     * We may have the VersionInfo here.
     */
    version = xmlParseVersionInfo(ctxt);
    if (version == NULL)
	version = xmlCharStrdup(XML_DEFAULT_VERSION);
    else {
	if (!IS_BLANK_CH(CUR)) {
	    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
		           "Space needed here\n");
	}
    }
    ctxt->input->version = version;

    /*
     * We must have the encoding declaration
     */
    xmlParseEncodingDecl(ctxt);
    if (ctxt->errNo == XML_ERR_UNSUPPORTED_ENCODING) {
	/*
	 * The XML REC instructs us to stop parsing right here
	 */
        return;
    }

    SKIP_BLANKS;
    if ((RAW == '?') && (NXT(1) == '>')) {
        SKIP(2);
    } else if (RAW == '>') {
        /* Deprecated old WD ... */
	xmlFatalErr(ctxt, XML_ERR_XMLDECL_NOT_FINISHED, NULL);
	NEXT;
    } else {
	xmlFatalErr(ctxt, XML_ERR_XMLDECL_NOT_FINISHED, NULL);
	MOVETO_ENDTAG(CUR_PTR);
	NEXT;
    }
}

/**
 * xmlParseExternalSubset:
 * @ctxt:  an XML parser context
 * @ExternalID: the external identifier
 * @SystemID: the system identifier (or URL)
 * 
 * parse Markup declarations from an external subset
 *
 * [30] extSubset ::= textDecl? extSubsetDecl
 *
 * [31] extSubsetDecl ::= (markupdecl | conditionalSect | PEReference | S) *
 */
void
xmlParseExternalSubset(xmlParserCtxtPtr ctxt, const xmlChar *ExternalID,
                       const xmlChar *SystemID) {
    xmlDetectSAX2(ctxt);
    GROW;
    if (CMP5(CUR_PTR, '<', '?', 'x', 'm', 'l')) {
	xmlParseTextDecl(ctxt);
	if (ctxt->errNo == XML_ERR_UNSUPPORTED_ENCODING) {
	    /*
	     * The XML REC instructs us to stop parsing right here
	     */
	    ctxt->instate = XML_PARSER_EOF;
	    return;
	}
    }
    if (ctxt->myDoc == NULL) {
        ctxt->myDoc = xmlNewDoc(BAD_CAST "1.0");
    }
    if ((ctxt->myDoc != NULL) && (ctxt->myDoc->intSubset == NULL))
        xmlCreateIntSubset(ctxt->myDoc, NULL, ExternalID, SystemID);

    ctxt->instate = XML_PARSER_DTD;
    ctxt->external = 1;
    while (((RAW == '<') && (NXT(1) == '?')) ||
           ((RAW == '<') && (NXT(1) == '!')) ||
	   (RAW == '%') || IS_BLANK_CH(CUR)) {
	const xmlChar *check = CUR_PTR;
	unsigned int cons = ctxt->input->consumed;

	GROW;
        if ((RAW == '<') && (NXT(1) == '!') && (NXT(2) == '[')) {
	    xmlParseConditionalSections(ctxt);
	} else if (IS_BLANK_CH(CUR)) {
	    NEXT;
	} else if (RAW == '%') {
            xmlParsePEReference(ctxt);
	} else
	    xmlParseMarkupDecl(ctxt);

	/*
	 * Pop-up of finished entities.
	 */
	while ((RAW == 0) && (ctxt->inputNr > 1))
	    xmlPopInput(ctxt);

	if ((CUR_PTR == check) && (cons == ctxt->input->consumed)) {
	    xmlFatalErr(ctxt, XML_ERR_EXT_SUBSET_NOT_FINISHED, NULL);
	    break;
	}
    }
    
    if (RAW != 0) {
	xmlFatalErr(ctxt, XML_ERR_EXT_SUBSET_NOT_FINISHED, NULL);
    }

}

/**
 * xmlParseReference:
 * @ctxt:  an XML parser context
 * 
 * parse and handle entity references in content, depending on the SAX
 * interface, this may end-up in a call to character() if this is a
 * CharRef, a predefined entity, if there is no reference() callback.
 * or if the parser was asked to switch to that mode.
 *
 * [67] Reference ::= EntityRef | CharRef
 */
void
xmlParseReference(xmlParserCtxtPtr ctxt) {
    xmlEntityPtr ent;
    xmlChar *val;
    if (RAW != '&') return;

    if (NXT(1) == '#') {
	int i = 0;
	xmlChar out[10];
	int hex = NXT(2);
	int value = xmlParseCharRef(ctxt);
	
	if (ctxt->charset != XML_CHAR_ENCODING_UTF8) {
	    /*
	     * So we are using non-UTF-8 buffers
	     * Check that the char fit on 8bits, if not
	     * generate a CharRef.
	     */
	    if (value <= 0xFF) {
		out[0] = value;
		out[1] = 0;
		if ((ctxt->sax != NULL) && (ctxt->sax->characters != NULL) &&
		    (!ctxt->disableSAX))
		    ctxt->sax->characters(ctxt->userData, out, 1);
	    } else {
		if ((hex == 'x') || (hex == 'X'))
		    snprintf((char *)out, sizeof(out), "#x%X", value);
		else
		    snprintf((char *)out, sizeof(out), "#%d", value);
		if ((ctxt->sax != NULL) && (ctxt->sax->reference != NULL) &&
		    (!ctxt->disableSAX))
		    ctxt->sax->reference(ctxt->userData, out);
	    }
	} else {
	    /*
	     * Just encode the value in UTF-8
	     */
	    COPY_BUF(0 ,out, i, value);
	    out[i] = 0;
	    if ((ctxt->sax != NULL) && (ctxt->sax->characters != NULL) &&
		(!ctxt->disableSAX))
		ctxt->sax->characters(ctxt->userData, out, i);
	}
    } else {
	ent = xmlParseEntityRef(ctxt);
	if (ent == NULL) return;
	if (!ctxt->wellFormed)
	    return;
	if ((ent->name != NULL) && 
	    (ent->etype != XML_INTERNAL_PREDEFINED_ENTITY)) {
	    xmlNodePtr list = NULL;
	    xmlParserErrors ret = XML_ERR_OK;


	    /*
	     * The first reference to the entity trigger a parsing phase
	     * where the ent->children is filled with the result from
	     * the parsing.
	     */
	    if (ent->children == NULL) {
		xmlChar *value;
		value = ent->content;

		/*
		 * Check that this entity is well formed
		 */
		if ((value != NULL) && (value[0] != 0) &&
		    (value[1] == 0) && (value[0] == '<') &&
		    (xmlStrEqual(ent->name, BAD_CAST "lt"))) {
		    /*
		     * DONE: get definite answer on this !!!
		     * Lots of entity decls are used to declare a single
		     * char 
		     *    <!ENTITY lt     "<">
		     * Which seems to be valid since
		     * 2.4: The ampersand character (&) and the left angle
		     * bracket (<) may appear in their literal form only
		     * when used ... They are also legal within the literal
		     * entity value of an internal entity declaration;i
		     * see "4.3.2 Well-Formed Parsed Entities". 
		     * IMHO 2.4 and 4.3.2 are directly in contradiction.
		     * Looking at the OASIS test suite and James Clark 
		     * tests, this is broken. However the XML REC uses
		     * it. Is the XML REC not well-formed ????
		     * This is a hack to avoid this problem
		     *
		     * ANSWER: since lt gt amp .. are already defined,
		     *   this is a redefinition and hence the fact that the
		     *   content is not well balanced is not a Wf error, this
		     *   is lousy but acceptable.
		     */
		    list = xmlNewDocText(ctxt->myDoc, value);
		    if (list != NULL) {
			if ((ent->etype == XML_INTERNAL_GENERAL_ENTITY) &&
			    (ent->children == NULL)) {
			    ent->children = list;
			    ent->last = list;
			    ent->owner = 1;
			    list->parent = (xmlNodePtr) ent;
			} else {
			    xmlFreeNodeList(list);
			}
		    } else if (list != NULL) {
			xmlFreeNodeList(list);
		    }
		} else {
		    /*
		     * 4.3.2: An internal general parsed entity is well-formed
		     * if its replacement text matches the production labeled
		     * content.
		     */

		    void *user_data;
		    /*
		     * This is a bit hackish but this seems the best
		     * way to make sure both SAX and DOM entity support
		     * behaves okay.
		     */
		    if (ctxt->userData == ctxt)
			user_data = NULL;
		    else
			user_data = ctxt->userData;

		    if (ent->etype == XML_INTERNAL_GENERAL_ENTITY) {
			ctxt->depth++;
			ret = xmlParseBalancedChunkMemoryInternal(ctxt,
					   value, user_data, &list);
			ctxt->depth--;
		    } else if (ent->etype ==
			       XML_EXTERNAL_GENERAL_PARSED_ENTITY) {
			ctxt->depth++;
			ret = xmlParseExternalEntityPrivate(ctxt->myDoc, ctxt,
				   ctxt->sax, user_data, ctxt->depth,
				   ent->URI, ent->ExternalID, &list);
			ctxt->depth--;
		    } else {
			ret = XML_ERR_ENTITY_PE_INTERNAL;
			xmlErrMsgStr(ctxt, XML_ERR_INTERNAL_ERROR,
				     "invalid entity type found\n", NULL);
		    }
		    if (ret == XML_ERR_ENTITY_LOOP) {
			xmlFatalErr(ctxt, XML_ERR_ENTITY_LOOP, NULL);
			return;
		    } else if ((ret == XML_ERR_OK) && (list != NULL)) {
			if (((ent->etype == XML_INTERNAL_GENERAL_ENTITY) ||
			 (ent->etype == XML_EXTERNAL_GENERAL_PARSED_ENTITY))&&
			    (ent->children == NULL)) {
			    ent->children = list;
			    if (ctxt->replaceEntities) {
				/*
				 * Prune it directly in the generated document
				 * except for single text nodes.
				 */
				if ((list->type == XML_TEXT_NODE) &&
				    (list->next == NULL)) {
				    list->parent = (xmlNodePtr) ent;
				    list = NULL;
				    ent->owner = 1;
				} else {
				    ent->owner = 0;
				    while (list != NULL) {
					list->parent = (xmlNodePtr) ctxt->node;
					list->doc = ctxt->myDoc;
					if (list->next == NULL)
					    ent->last = list;
					list = list->next;
			 	    }
				    list = ent->children;
#ifdef LIBXML_LEGACY_ENABLED
				    if (ent->etype == XML_EXTERNAL_GENERAL_PARSED_ENTITY)
				      xmlAddEntityReference(ent, list, NULL);
#endif /* LIBXML_LEGACY_ENABLED */
				}
			    } else {
				ent->owner = 1;
				while (list != NULL) {
				    list->parent = (xmlNodePtr) ent;
				    if (list->next == NULL)
					ent->last = list;
				    list = list->next;
				}
			    }
			} else {
			    xmlFreeNodeList(list);
			    list = NULL;
			}
		    } else if ((ret != XML_ERR_OK) &&
		               (ret != XML_WAR_UNDECLARED_ENTITY)) {
			xmlFatalErr(ctxt, ret, NULL);
		    } else if (list != NULL) {
			xmlFreeNodeList(list);
			list = NULL;
		    }
		}
	    }
	    if ((ctxt->sax != NULL) && (ctxt->sax->reference != NULL) &&
		(ctxt->replaceEntities == 0) && (!ctxt->disableSAX)) {
		/*
		 * Create a node.
		 */
		ctxt->sax->reference(ctxt->userData, ent->name);
		return;
	    } else if (ctxt->replaceEntities) {
		if ((ctxt->node != NULL) && (ent->children != NULL)) {
		    /*
		     * Seems we are generating the DOM content, do
		     * a simple tree copy for all references except the first
		     * In the first occurrence list contains the replacement
		     */
		    if ((list == NULL) && (ent->owner == 0)) {
			xmlNodePtr nw = NULL, cur, firstChild = NULL;
			cur = ent->children;
			while (cur != NULL) {
			    nw = xmlCopyNode(cur, 1);
			    if (nw != NULL) {
				nw->_private = cur->_private;
				if (firstChild == NULL){
				    firstChild = nw;
				}
				xmlAddChild(ctxt->node, nw);
			    }
			    if (cur == ent->last)
				break;
			    cur = cur->next;
			}
#ifdef LIBXML_LEGACY_ENABLED
			if (ent->etype == XML_EXTERNAL_GENERAL_PARSED_ENTITY)			      
			  xmlAddEntityReference(ent, firstChild, nw);
#endif /* LIBXML_LEGACY_ENABLED */
		    } else if (list == NULL) {
			xmlNodePtr nw = NULL, cur, next, last,
			           firstChild = NULL;
			/*
			 * Copy the entity child list and make it the new
			 * entity child list. The goal is to make sure any
			 * ID or REF referenced will be the one from the
			 * document content and not the entity copy.
			 */
			cur = ent->children;
			ent->children = NULL;
			last = ent->last;
			ent->last = NULL;
			while (cur != NULL) {
			    next = cur->next;
			    cur->next = NULL;
			    cur->parent = NULL;
			    nw = xmlCopyNode(cur, 1);
			    if (nw != NULL) {
				nw->_private = cur->_private;
				if (firstChild == NULL){
				    firstChild = cur;
				}
				xmlAddChild((xmlNodePtr) ent, nw);
				xmlAddChild(ctxt->node, cur);
			    }
			    if (cur == last)
				break;
			    cur = next;
			}
			ent->owner = 1;
#ifdef LIBXML_LEGACY_ENABLED
			if (ent->etype == XML_EXTERNAL_GENERAL_PARSED_ENTITY)			      
			  xmlAddEntityReference(ent, firstChild, nw);
#endif /* LIBXML_LEGACY_ENABLED */
		    } else {
			/*
			 * the name change is to avoid coalescing of the
			 * node with a possible previous text one which
			 * would make ent->children a dangling pointer
			 */
			if (ent->children->type == XML_TEXT_NODE)
			    ent->children->name = xmlStrdup(BAD_CAST "nbktext");
			if ((ent->last != ent->children) &&
			    (ent->last->type == XML_TEXT_NODE))
			    ent->last->name = xmlStrdup(BAD_CAST "nbktext");
			xmlAddChildList(ctxt->node, ent->children);
		    }

		    /*
		     * This is to avoid a nasty side effect, see
		     * characters() in SAX.c
		     */
		    ctxt->nodemem = 0;
		    ctxt->nodelen = 0;
		    return;
		} else {
		    /*
		     * Probably running in SAX mode
		     */
		    xmlParserInputPtr input;

		    input = xmlNewEntityInputStream(ctxt, ent);
		    xmlPushInput(ctxt, input);
		    if ((ent->etype == XML_EXTERNAL_GENERAL_PARSED_ENTITY) &&
			(CMP5(CUR_PTR, '<', '?', 'x', 'm', 'l')) &&
			(IS_BLANK_CH(NXT(5)))) {
			xmlParseTextDecl(ctxt);
			if (ctxt->errNo == XML_ERR_UNSUPPORTED_ENCODING) {
			    /*
			     * The XML REC instructs us to stop parsing right here
			     */
			    ctxt->instate = XML_PARSER_EOF;
			    return;
			}
			if (input->standalone == 1) {
			    xmlFatalErr(ctxt, XML_ERR_EXT_ENTITY_STANDALONE,
			                NULL);
			}
		    }
		    return;
		}
	    }
	} else {
	    val = ent->content;
	    if (val == NULL) return;
	    /*
	     * inline the entity.
	     */
	    if ((ctxt->sax != NULL) && (ctxt->sax->characters != NULL) &&
		(!ctxt->disableSAX))
		ctxt->sax->characters(ctxt->userData, val, xmlStrlen(val));
	}
    }
}

/**
 * xmlParseEntityRef:
 * @ctxt:  an XML parser context
 *
 * parse ENTITY references declarations
 *
 * [68] EntityRef ::= '&' Name ';'
 *
 * [ WFC: Entity Declared ]
 * In a document without any DTD, a document with only an internal DTD
 * subset which contains no parameter entity references, or a document
 * with "standalone='yes'", the Name given in the entity reference
 * must match that in an entity declaration, except that well-formed
 * documents need not declare any of the following entities: amp, lt,
 * gt, apos, quot.  The declaration of a parameter entity must precede
 * any reference to it.  Similarly, the declaration of a general entity
 * must precede any reference to it which appears in a default value in an
 * attribute-list declaration. Note that if entities are declared in the
 * external subset or in external parameter entities, a non-validating
 * processor is not obligated to read and process their declarations;
 * for such documents, the rule that an entity must be declared is a
 * well-formedness constraint only if standalone='yes'.
 *
 * [ WFC: Parsed Entity ]
 * An entity reference must not contain the name of an unparsed entity
 *
 * Returns the xmlEntityPtr if found, or NULL otherwise.
 */
xmlEntityPtr
xmlParseEntityRef(xmlParserCtxtPtr ctxt) {
    const xmlChar *name;
    xmlEntityPtr ent = NULL;

    GROW;
    
    if (RAW == '&') {
        NEXT;
        name = xmlParseName(ctxt);
	if (name == NULL) {
	    xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED,
			   "xmlParseEntityRef: no name\n");
	} else {
	    if (RAW == ';') {
	        NEXT;
		/*
		 * Ask first SAX for entity resolution, otherwise try the
		 * predefined set.
		 */
		if (ctxt->sax != NULL) {
		    if (ctxt->sax->getEntity != NULL)
			ent = ctxt->sax->getEntity(ctxt->userData, name);
		    if ((ctxt->wellFormed == 1 ) && (ent == NULL))
		        ent = xmlGetPredefinedEntity(name);
		    if ((ctxt->wellFormed == 1 ) && (ent == NULL) &&
			(ctxt->userData==ctxt)) {
			ent = xmlSAX2GetEntity(ctxt, name);
		    }
		}
		/*
		 * [ WFC: Entity Declared ]
		 * In a document without any DTD, a document with only an
		 * internal DTD subset which contains no parameter entity
		 * references, or a document with "standalone='yes'", the
		 * Name given in the entity reference must match that in an
		 * entity declaration, except that well-formed documents
		 * need not declare any of the following entities: amp, lt,
		 * gt, apos, quot.
		 * The declaration of a parameter entity must precede any
		 * reference to it.
		 * Similarly, the declaration of a general entity must
		 * precede any reference to it which appears in a default
		 * value in an attribute-list declaration. Note that if
		 * entities are declared in the external subset or in
		 * external parameter entities, a non-validating processor
		 * is not obligated to read and process their declarations;
		 * for such documents, the rule that an entity must be
		 * declared is a well-formedness constraint only if
		 * standalone='yes'. 
		 */
		if (ent == NULL) {
		    if ((ctxt->standalone == 1) ||
		        ((ctxt->hasExternalSubset == 0) &&
			 (ctxt->hasPErefs == 0))) {
			xmlFatalErrMsgStr(ctxt, XML_ERR_UNDECLARED_ENTITY,
				 "Entity '%s' not defined\n", name);
		    } else {
		        xmlErrMsgStr(ctxt, XML_WAR_UNDECLARED_ENTITY,
				 "Entity '%s' not defined\n", name);
		    }
		    ctxt->valid = 0;
		}

		/*
		 * [ WFC: Parsed Entity ]
		 * An entity reference must not contain the name of an
		 * unparsed entity
		 */
		else if (ent->etype == XML_EXTERNAL_GENERAL_UNPARSED_ENTITY) {
		    xmlFatalErrMsgStr(ctxt, XML_ERR_UNPARSED_ENTITY,
			     "Entity reference to unparsed entity %s\n", name);
		}

		/*
		 * [ WFC: No External Entity References ]
		 * Attribute values cannot contain direct or indirect
		 * entity references to external entities.
		 */
		else if ((ctxt->instate == XML_PARSER_ATTRIBUTE_VALUE) &&
		         (ent->etype == XML_EXTERNAL_GENERAL_PARSED_ENTITY)) {
		    xmlFatalErrMsgStr(ctxt, XML_ERR_ENTITY_IS_EXTERNAL,
			 "Attribute references external entity '%s'\n", name);
		}
		/*
		 * [ WFC: No < in Attribute Values ]
		 * The replacement text of any entity referred to directly or
		 * indirectly in an attribute value (other than "&lt;") must
		 * not contain a <. 
		 */
		else if ((ctxt->instate == XML_PARSER_ATTRIBUTE_VALUE) &&
		         (ent != NULL) &&
			 (!xmlStrEqual(ent->name, BAD_CAST "lt")) &&
		         (ent->content != NULL) &&
			 (xmlStrchr(ent->content, '<'))) {
		    xmlFatalErrMsgStr(ctxt, XML_ERR_LT_IN_ATTRIBUTE,
	 "'<' in entity '%s' is not allowed in attributes values\n", name);
		}

		/*
		 * Internal check, no parameter entities here ...
		 */
		else {
		    switch (ent->etype) {
			case XML_INTERNAL_PARAMETER_ENTITY:
			case XML_EXTERNAL_PARAMETER_ENTITY:
			xmlFatalErrMsgStr(ctxt, XML_ERR_ENTITY_IS_PARAMETER,
			 "Attempt to reference the parameter entity '%s'\n",
		                          name);
			break;
			default:
			break;
		    }
		}

		/*
		 * [ WFC: No Recursion ]
		 * A parsed entity must not contain a recursive reference
		 * to itself, either directly or indirectly. 
		 * Done somewhere else
		 */

	    } else {
		xmlFatalErr(ctxt, XML_ERR_ENTITYREF_SEMICOL_MISSING, NULL);
	    }
	}
    }
    return(ent);
}

/**
 * xmlParseStringEntityRef:
 * @ctxt:  an XML parser context
 * @str:  a pointer to an index in the string
 *
 * parse ENTITY references declarations, but this version parses it from
 * a string value.
 *
 * [68] EntityRef ::= '&' Name ';'
 *
 * [ WFC: Entity Declared ]
 * In a document without any DTD, a document with only an internal DTD
 * subset which contains no parameter entity references, or a document
 * with "standalone='yes'", the Name given in the entity reference
 * must match that in an entity declaration, except that well-formed
 * documents need not declare any of the following entities: amp, lt,
 * gt, apos, quot.  The declaration of a parameter entity must precede
 * any reference to it.  Similarly, the declaration of a general entity
 * must precede any reference to it which appears in a default value in an
 * attribute-list declaration. Note that if entities are declared in the
 * external subset or in external parameter entities, a non-validating
 * processor is not obligated to read and process their declarations;
 * for such documents, the rule that an entity must be declared is a
 * well-formedness constraint only if standalone='yes'.
 *
 * [ WFC: Parsed Entity ]
 * An entity reference must not contain the name of an unparsed entity
 *
 * Returns the xmlEntityPtr if found, or NULL otherwise. The str pointer
 * is updated to the current location in the string.
 */
xmlEntityPtr
xmlParseStringEntityRef(xmlParserCtxtPtr ctxt, const xmlChar ** str) {
    xmlChar *name;
    const xmlChar *ptr;
    xmlChar cur;
    xmlEntityPtr ent = NULL;

    if ((str == NULL) || (*str == NULL))
        return(NULL);
    ptr = *str;
    cur = *ptr;
    if (cur == '&') {
        ptr++;
	cur = *ptr;
        name = xmlParseStringName(ctxt, &ptr);
	if (name == NULL) {
	    xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED,
			   "xmlParseStringEntityRef: no name\n");
	} else {
	    if (*ptr == ';') {
	        ptr++;
		/*
		 * Ask first SAX for entity resolution, otherwise try the
		 * predefined set.
		 */
		if (ctxt->sax != NULL) {
		    if (ctxt->sax->getEntity != NULL)
			ent = ctxt->sax->getEntity(ctxt->userData, name);
		    if (ent == NULL)
		        ent = xmlGetPredefinedEntity(name);
		    if ((ent == NULL) && (ctxt->userData==ctxt)) {
			ent = xmlSAX2GetEntity(ctxt, name);
		    }
		}
		/*
		 * [ WFC: Entity Declared ]
		 * In a document without any DTD, a document with only an
		 * internal DTD subset which contains no parameter entity
		 * references, or a document with "standalone='yes'", the
		 * Name given in the entity reference must match that in an
		 * entity declaration, except that well-formed documents
		 * need not declare any of the following entities: amp, lt,
		 * gt, apos, quot.
		 * The declaration of a parameter entity must precede any
		 * reference to it.
		 * Similarly, the declaration of a general entity must
		 * precede any reference to it which appears in a default
		 * value in an attribute-list declaration. Note that if
		 * entities are declared in the external subset or in
		 * external parameter entities, a non-validating processor
		 * is not obligated to read and process their declarations;
		 * for such documents, the rule that an entity must be
		 * declared is a well-formedness constraint only if
		 * standalone='yes'. 
		 */
		if (ent == NULL) {
		    if ((ctxt->standalone == 1) ||
		        ((ctxt->hasExternalSubset == 0) &&
			 (ctxt->hasPErefs == 0))) {
			xmlFatalErrMsgStr(ctxt, XML_ERR_UNDECLARED_ENTITY,
				 "Entity '%s' not defined\n", name);
		    } else {
			xmlErrMsgStr(ctxt, XML_WAR_UNDECLARED_ENTITY,
				      "Entity '%s' not defined\n",
				      name);
		    }
		    /* TODO ? check regressions ctxt->valid = 0; */
		}

		/*
		 * [ WFC: Parsed Entity ]
		 * An entity reference must not contain the name of an
		 * unparsed entity
		 */
		else if (ent->etype == XML_EXTERNAL_GENERAL_UNPARSED_ENTITY) {
		    xmlFatalErrMsgStr(ctxt, XML_ERR_UNPARSED_ENTITY,
			     "Entity reference to unparsed entity %s\n", name);
		}

		/*
		 * [ WFC: No External Entity References ]
		 * Attribute values cannot contain direct or indirect
		 * entity references to external entities.
		 */
		else if ((ctxt->instate == XML_PARSER_ATTRIBUTE_VALUE) &&
		         (ent->etype == XML_EXTERNAL_GENERAL_PARSED_ENTITY)) {
		    xmlFatalErrMsgStr(ctxt, XML_ERR_ENTITY_IS_EXTERNAL,
		     "Attribute references external entity '%s'\n", name);
		}
		/*
		 * [ WFC: No < in Attribute Values ]
		 * The replacement text of any entity referred to directly or
		 * indirectly in an attribute value (other than "&lt;") must
		 * not contain a <. 
		 */
		else if ((ctxt->instate == XML_PARSER_ATTRIBUTE_VALUE) &&
		         (ent != NULL) &&
			 (!xmlStrEqual(ent->name, BAD_CAST "lt")) &&
		         (ent->content != NULL) &&
			 (xmlStrchr(ent->content, '<'))) {
		    xmlFatalErrMsgStr(ctxt, XML_ERR_LT_IN_ATTRIBUTE,
		 "'<' in entity '%s' is not allowed in attributes values\n",
	                              name);
		}

		/*
		 * Internal check, no parameter entities here ...
		 */
		else {
		    switch (ent->etype) {
			case XML_INTERNAL_PARAMETER_ENTITY:
			case XML_EXTERNAL_PARAMETER_ENTITY:
			    xmlFatalErrMsgStr(ctxt, XML_ERR_ENTITY_IS_PARAMETER,
			 "Attempt to reference the parameter entity '%s'\n",
			                      name);
			break;
			default:
			break;
		    }
		}

		/*
		 * [ WFC: No Recursion ]
		 * A parsed entity must not contain a recursive reference
		 * to itself, either directly or indirectly. 
		 * Done somewhere else
		 */

	    } else {
		xmlFatalErr(ctxt, XML_ERR_ENTITYREF_SEMICOL_MISSING, NULL);
	    }
	    xmlFree(name);
	}
    }
    *str = ptr;
    return(ent);
}

/**
 * xmlParsePEReference:
 * @ctxt:  an XML parser context
 *
 * parse PEReference declarations
 * The entity content is handled directly by pushing it's content as
 * a new input stream.
 *
 * [69] PEReference ::= '%' Name ';'
 *
 * [ WFC: No Recursion ]
 * A parsed entity must not contain a recursive
 * reference to itself, either directly or indirectly. 
 *
 * [ WFC: Entity Declared ]
 * In a document without any DTD, a document with only an internal DTD
 * subset which contains no parameter entity references, or a document
 * with "standalone='yes'", ...  ... The declaration of a parameter
 * entity must precede any reference to it...
 *
 * [ VC: Entity Declared ]
 * In a document with an external subset or external parameter entities
 * with "standalone='no'", ...  ... The declaration of a parameter entity
 * must precede any reference to it...
 *
 * [ WFC: In DTD ]
 * Parameter-entity references may only appear in the DTD.
 * NOTE: misleading but this is handled.
 */
void
xmlParsePEReference(xmlParserCtxtPtr ctxt)
{
    const xmlChar *name;
    xmlEntityPtr entity = NULL;
    xmlParserInputPtr input;

    if (RAW == '%') {
        NEXT;
        name = xmlParseName(ctxt);
        if (name == NULL) {
            xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED,
                           "xmlParsePEReference: no name\n");
        } else {
            if (RAW == ';') {
                NEXT;
                if ((ctxt->sax != NULL) &&
                    (ctxt->sax->getParameterEntity != NULL))
                    entity = ctxt->sax->getParameterEntity(ctxt->userData,
                                                           name);
                if (entity == NULL) {
                    /*
                     * [ WFC: Entity Declared ]
                     * In a document without any DTD, a document with only an
                     * internal DTD subset which contains no parameter entity
                     * references, or a document with "standalone='yes'", ...
                     * ... The declaration of a parameter entity must precede
                     * any reference to it...
                     */
                    if ((ctxt->standalone == 1) ||
                        ((ctxt->hasExternalSubset == 0) &&
                         (ctxt->hasPErefs == 0))) {
                        xmlFatalErrMsgStr(ctxt, XML_ERR_UNDECLARED_ENTITY,
                                          "PEReference: %%%s; not found\n",
                                          name);
                    } else {
                        /*
                         * [ VC: Entity Declared ]
                         * In a document with an external subset or external
                         * parameter entities with "standalone='no'", ...
                         * ... The declaration of a parameter entity must
			 * precede any reference to it...
                         */
                        xmlWarningMsg(ctxt, XML_WAR_UNDECLARED_ENTITY,
                                      "PEReference: %%%s; not found\n",
                                      name, NULL);
                        ctxt->valid = 0;
                    }
                } else {
                    /*
                     * Internal checking in case the entity quest barfed
                     */
                    if ((entity->etype != XML_INTERNAL_PARAMETER_ENTITY) &&
                        (entity->etype != XML_EXTERNAL_PARAMETER_ENTITY)) {
                        xmlWarningMsg(ctxt, XML_WAR_UNDECLARED_ENTITY,
			      "Internal: %%%s; is not a parameter entity\n",
                                      name, NULL);
                    } else if (ctxt->input->free != deallocblankswrapper) {
                        input =
                            xmlNewBlanksWrapperInputStream(ctxt, entity);
                        xmlPushInput(ctxt, input);
                    } else {
                        /*
                         * TODO !!!
                         * handle the extra spaces added before and after
                         * c.f. http://www.w3.org/TR/REC-xml#as-PE
                         */
                        input = xmlNewEntityInputStream(ctxt, entity);
                        xmlPushInput(ctxt, input);
                        if ((entity->etype == XML_EXTERNAL_PARAMETER_ENTITY) &&
			    (CMP5(CUR_PTR, '<', '?', 'x', 'm', 'l')) &&
			    (IS_BLANK_CH(NXT(5)))) {
                            xmlParseTextDecl(ctxt);
                            if (ctxt->errNo ==
                                XML_ERR_UNSUPPORTED_ENCODING) {
                                /*
                                 * The XML REC instructs us to stop parsing
                                 * right here
                                 */
                                ctxt->instate = XML_PARSER_EOF;
                                return;
                            }
                        }
                    }
                }
                ctxt->hasPErefs = 1;
            } else {
                xmlFatalErr(ctxt, XML_ERR_ENTITYREF_SEMICOL_MISSING, NULL);
            }
        }
    }
}

/**
 * xmlParseStringPEReference:
 * @ctxt:  an XML parser context
 * @str:  a pointer to an index in the string
 *
 * parse PEReference declarations
 *
 * [69] PEReference ::= '%' Name ';'
 *
 * [ WFC: No Recursion ]
 * A parsed entity must not contain a recursive
 * reference to itself, either directly or indirectly. 
 *
 * [ WFC: Entity Declared ]
 * In a document without any DTD, a document with only an internal DTD
 * subset which contains no parameter entity references, or a document
 * with "standalone='yes'", ...  ... The declaration of a parameter
 * entity must precede any reference to it...
 *
 * [ VC: Entity Declared ]
 * In a document with an external subset or external parameter entities
 * with "standalone='no'", ...  ... The declaration of a parameter entity
 * must precede any reference to it...
 *
 * [ WFC: In DTD ]
 * Parameter-entity references may only appear in the DTD.
 * NOTE: misleading but this is handled.
 *
 * Returns the string of the entity content.
 *         str is updated to the current value of the index
 */
xmlEntityPtr
xmlParseStringPEReference(xmlParserCtxtPtr ctxt, const xmlChar **str) {
    const xmlChar *ptr;
    xmlChar cur;
    xmlChar *name;
    xmlEntityPtr entity = NULL;

    if ((str == NULL) || (*str == NULL)) return(NULL);
    ptr = *str;
    cur = *ptr;
    if (cur == '%') {
        ptr++;
	cur = *ptr;
        name = xmlParseStringName(ctxt, &ptr);
	if (name == NULL) {
	    xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED,
			   "xmlParseStringPEReference: no name\n");
	} else {
	    cur = *ptr;
	    if (cur == ';') {
		ptr++;
		cur = *ptr;
		if ((ctxt->sax != NULL) &&
		    (ctxt->sax->getParameterEntity != NULL))
		    entity = ctxt->sax->getParameterEntity(ctxt->userData,
		                                           name);
		if (entity == NULL) {
		    /*
		     * [ WFC: Entity Declared ]
		     * In a document without any DTD, a document with only an
		     * internal DTD subset which contains no parameter entity
		     * references, or a document with "standalone='yes'", ...
		     * ... The declaration of a parameter entity must precede
		     * any reference to it...
		     */
		    if ((ctxt->standalone == 1) ||
			((ctxt->hasExternalSubset == 0) &&
			 (ctxt->hasPErefs == 0))) {
			xmlFatalErrMsgStr(ctxt, XML_ERR_UNDECLARED_ENTITY,
			     "PEReference: %%%s; not found\n", name);
		    } else {
			/*
			 * [ VC: Entity Declared ]
			 * In a document with an external subset or external
			 * parameter entities with "standalone='no'", ...
			 * ... The declaration of a parameter entity must
			 * precede any reference to it...
			 */
			xmlWarningMsg(ctxt, XML_WAR_UNDECLARED_ENTITY,
				      "PEReference: %%%s; not found\n",
			              name, NULL);
			ctxt->valid = 0;
		    }
		} else {
		    /*
		     * Internal checking in case the entity quest barfed
		     */
		    if ((entity->etype != XML_INTERNAL_PARAMETER_ENTITY) &&
		        (entity->etype != XML_EXTERNAL_PARAMETER_ENTITY)) {
			xmlWarningMsg(ctxt, XML_WAR_UNDECLARED_ENTITY,
			              "%%%s; is not a parameter entity\n",
				      name, NULL);
		    }
		}
		ctxt->hasPErefs = 1;
	    } else {
		xmlFatalErr(ctxt, XML_ERR_ENTITYREF_SEMICOL_MISSING, NULL);
	    }
	    xmlFree(name);
	}
    }
    *str = ptr;
    return(entity);
}

/**
 * xmlParseDocTypeDecl:
 * @ctxt:  an XML parser context
 *
 * parse a DOCTYPE declaration
 *
 * [28] doctypedecl ::= '<!DOCTYPE' S Name (S ExternalID)? S? 
 *                      ('[' (markupdecl | PEReference | S)* ']' S?)? '>'
 *
 * [ VC: Root Element Type ]
 * The Name in the document type declaration must match the element
 * type of the root element. 
 */

void
xmlParseDocTypeDecl(xmlParserCtxtPtr ctxt) {
    const xmlChar *name = NULL;
    xmlChar *ExternalID = NULL;
    xmlChar *URI = NULL;

    /*
     * We know that '<!DOCTYPE' has been detected.
     */
    SKIP(9);

    SKIP_BLANKS;

    /*
     * Parse the DOCTYPE name.
     */
    name = xmlParseName(ctxt);
    if (name == NULL) {
	xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED,
		       "xmlParseDocTypeDecl : no DOCTYPE name !\n");
    }
    ctxt->intSubName = name;

    SKIP_BLANKS;

    /*
     * Check for SystemID and ExternalID
     */
    URI = xmlParseExternalID(ctxt, &ExternalID, 1);

    if ((URI != NULL) || (ExternalID != NULL)) {
        ctxt->hasExternalSubset = 1;
    }
    ctxt->extSubURI = URI;
    ctxt->extSubSystem = ExternalID;

    SKIP_BLANKS;

    /*
     * Create and update the internal subset.
     */
    if ((ctxt->sax != NULL) && (ctxt->sax->internalSubset != NULL) &&
	(!ctxt->disableSAX))
	ctxt->sax->internalSubset(ctxt->userData, name, ExternalID, URI);

    /*
     * Is there any internal subset declarations ?
     * they are handled separately in xmlParseInternalSubset()
     */
    if (RAW == '[')
	return;

    /*
     * We should be at the end of the DOCTYPE declaration.
     */
    if (RAW != '>') {
	xmlFatalErr(ctxt, XML_ERR_DOCTYPE_NOT_FINISHED, NULL);
    }
    NEXT;
}

/**
 * xmlParseInternalSubset:
 * @ctxt:  an XML parser context
 *
 * parse the internal subset declaration
 *
 * [28 end] ('[' (markupdecl | PEReference | S)* ']' S?)? '>'
 */

static void
xmlParseInternalSubset(xmlParserCtxtPtr ctxt) {
    /*
     * Is there any DTD definition ?
     */
    if (RAW == '[') {
        ctxt->instate = XML_PARSER_DTD;
        NEXT;
	/*
	 * Parse the succession of Markup declarations and 
	 * PEReferences.
	 * Subsequence (markupdecl | PEReference | S)*
	 */
	while (RAW != ']') {
	    const xmlChar *check = CUR_PTR;
	    unsigned int cons = ctxt->input->consumed;

	    SKIP_BLANKS;
	    xmlParseMarkupDecl(ctxt);
	    xmlParsePEReference(ctxt);

	    /*
	     * Pop-up of finished entities.
	     */
	    while ((RAW == 0) && (ctxt->inputNr > 1))
		xmlPopInput(ctxt);

	    if ((CUR_PTR == check) && (cons == ctxt->input->consumed)) {
		xmlFatalErr(ctxt, XML_ERR_INTERNAL_ERROR,
	     "xmlParseInternalSubset: error detected in Markup declaration\n");
		break;
	    }
	}
	if (RAW == ']') { 
	    NEXT;
	    SKIP_BLANKS;
	}
    }

    /*
     * We should be at the end of the DOCTYPE declaration.
     */
    if (RAW != '>') {
	xmlFatalErr(ctxt, XML_ERR_DOCTYPE_NOT_FINISHED, NULL);
    }
    NEXT;
}

#ifdef LIBXML_SAX1_ENABLED
/**
 * xmlParseAttribute:
 * @ctxt:  an XML parser context
 * @value:  a xmlChar ** used to store the value of the attribute
 *
 * parse an attribute
 *
 * [41] Attribute ::= Name Eq AttValue
 *
 * [ WFC: No External Entity References ]
 * Attribute values cannot contain direct or indirect entity references
 * to external entities.
 *
 * [ WFC: No < in Attribute Values ]
 * The replacement text of any entity referred to directly or indirectly in
 * an attribute value (other than "&lt;") must not contain a <. 
 * 
 * [ VC: Attribute Value Type ]
 * The attribute must have been declared; the value must be of the type
 * declared for it.
 *
 * [25] Eq ::= S? '=' S?
 *
 * With namespace:
 *
 * [NS 11] Attribute ::= QName Eq AttValue
 *
 * Also the case QName == xmlns:??? is handled independently as a namespace
 * definition.
 *
 * Returns the attribute name, and the value in *value.
 */

const xmlChar *
xmlParseAttribute(xmlParserCtxtPtr ctxt, xmlChar **value) {
    const xmlChar *name;
    xmlChar *val;

    *value = NULL;
    GROW;
    name = xmlParseName(ctxt);
    if (name == NULL) {
	xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED,
	               "error parsing attribute name\n");
        return(NULL);
    }

    /*
     * read the value
     */
    SKIP_BLANKS;
    if (RAW == '=') {
        NEXT;
	SKIP_BLANKS;
	val = xmlParseAttValue(ctxt);
	ctxt->instate = XML_PARSER_CONTENT;
    } else {
	xmlFatalErrMsgStr(ctxt, XML_ERR_ATTRIBUTE_WITHOUT_VALUE,
	       "Specification mandate value for attribute %s\n", name);
	return(NULL);
    }

    /*
     * Check that xml:lang conforms to the specification
     * No more registered as an error, just generate a warning now
     * since this was deprecated in XML second edition
     */
    if ((ctxt->pedantic) && (xmlStrEqual(name, BAD_CAST "xml:lang"))) {
	if (!xmlCheckLanguageID(val)) {
	    xmlWarningMsg(ctxt, XML_WAR_LANG_VALUE,
		          "Malformed value for xml:lang : %s\n",
			  val, NULL);
	}
    }

    /*
     * Check that xml:space conforms to the specification
     */
    if (xmlStrEqual(name, BAD_CAST "xml:space")) {
	if (xmlStrEqual(val, BAD_CAST "default"))
	    *(ctxt->space) = 0;
	else if (xmlStrEqual(val, BAD_CAST "preserve"))
	    *(ctxt->space) = 1;
	else {
	    xmlFatalErrMsgStr(ctxt, XML_ERR_ATTRIBUTE_WITHOUT_VALUE,
"Invalid value \"%s\" for xml:space : \"default\" or \"preserve\" expected\n",
                                 val);
	}
    }

    *value = val;
    return(name);
}

/**
 * xmlParseStartTag:
 * @ctxt:  an XML parser context
 * 
 * parse a start of tag either for rule element or
 * EmptyElement. In both case we don't parse the tag closing chars.
 *
 * [40] STag ::= '<' Name (S Attribute)* S? '>'
 *
 * [ WFC: Unique Att Spec ]
 * No attribute name may appear more than once in the same start-tag or
 * empty-element tag. 
 *
 * [44] EmptyElemTag ::= '<' Name (S Attribute)* S? '/>'
 *
 * [ WFC: Unique Att Spec ]
 * No attribute name may appear more than once in the same start-tag or
 * empty-element tag. 
 *
 * With namespace:
 *
 * [NS 8] STag ::= '<' QName (S Attribute)* S? '>'
 *
 * [NS 10] EmptyElement ::= '<' QName (S Attribute)* S? '/>'
 *
 * Returns the element name parsed
 */

const xmlChar *
xmlParseStartTag(xmlParserCtxtPtr ctxt) {
    const xmlChar *name;
    const xmlChar *attname;
    xmlChar *attvalue;
    const xmlChar **atts = ctxt->atts;
    int nbatts = 0;
    int maxatts = ctxt->maxatts;
    int i;

    if (RAW != '<') return(NULL);
    NEXT1;

    name = xmlParseName(ctxt);
    if (name == NULL) {
	xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED,
	     "xmlParseStartTag: invalid element name\n");
        return(NULL);
    }

    /*
     * Now parse the attributes, it ends up with the ending
     *
     * (S Attribute)* S?
     */
    SKIP_BLANKS;
    GROW;

    while ((RAW != '>') && 
	   ((RAW != '/') || (NXT(1) != '>')) &&
	   (IS_BYTE_CHAR(RAW))) {
	const xmlChar *q = CUR_PTR;
	unsigned int cons = ctxt->input->consumed;

	attname = xmlParseAttribute(ctxt, &attvalue);
        if ((attname != NULL) && (attvalue != NULL)) {
	    /*
	     * [ WFC: Unique Att Spec ]
	     * No attribute name may appear more than once in the same
	     * start-tag or empty-element tag. 
	     */
	    for (i = 0; i < nbatts;i += 2) {
	        if (xmlStrEqual(atts[i], attname)) {
		    xmlErrAttributeDup(ctxt, NULL, attname);
		    xmlFree(attvalue);
		    goto failed;
		}
	    }
	    /*
	     * Add the pair to atts
	     */
	    if (atts == NULL) {
	        maxatts = 22; /* allow for 10 attrs by default */
	        atts = (const xmlChar **)
		       xmlMalloc(maxatts * sizeof(xmlChar *));
		if (atts == NULL) {
		    xmlErrMemory(ctxt, NULL);
		    if (attvalue != NULL)
			xmlFree(attvalue);
		    goto failed;
		}
		ctxt->atts = atts;
		ctxt->maxatts = maxatts;
	    } else if (nbatts + 4 > maxatts) {
	        const xmlChar **n;

	        maxatts *= 2;
	        n = (const xmlChar **) xmlRealloc((void *) atts,
					     maxatts * sizeof(const xmlChar *));
		if (n == NULL) {
		    xmlErrMemory(ctxt, NULL);
		    if (attvalue != NULL)
			xmlFree(attvalue);
		    goto failed;
		}
		atts = n;
		ctxt->atts = atts;
		ctxt->maxatts = maxatts;
	    }
	    atts[nbatts++] = attname;
	    atts[nbatts++] = attvalue;
	    atts[nbatts] = NULL;
	    atts[nbatts + 1] = NULL;
	} else {
	    if (attvalue != NULL)
		xmlFree(attvalue);
	}

failed:     

	GROW
	if ((RAW == '>') || (((RAW == '/') && (NXT(1) == '>'))))
	    break;
	if (!IS_BLANK_CH(RAW)) {
	    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
			   "attributes construct error\n");
	}
	SKIP_BLANKS;
        if ((cons == ctxt->input->consumed) && (q == CUR_PTR) &&
            (attname == NULL) && (attvalue == NULL)) {
	    xmlFatalErrMsg(ctxt, XML_ERR_INTERNAL_ERROR,
			   "xmlParseStartTag: problem parsing attributes\n");
	    break;
	}
	SHRINK;
        GROW;
    }

    /*
     * SAX: Start of Element !
     */
    if ((ctxt->sax != NULL) && (ctxt->sax->startElement != NULL) &&
	(!ctxt->disableSAX)) {
	if (nbatts > 0)
	    ctxt->sax->startElement(ctxt->userData, name, atts);
	else
	    ctxt->sax->startElement(ctxt->userData, name, NULL);
    }

    if (atts != NULL) {
        /* Free only the content strings */
        for (i = 1;i < nbatts;i+=2)
	    if (atts[i] != NULL)
	       xmlFree((xmlChar *) atts[i]);
    }
    return(name);
}

/**
 * xmlParseEndTag1:
 * @ctxt:  an XML parser context
 * @line:  line of the start tag
 * @nsNr:  number of namespaces on the start tag
 *
 * parse an end of tag
 *
 * [42] ETag ::= '</' Name S? '>'
 *
 * With namespace
 *
 * [NS 9] ETag ::= '</' QName S? '>'
 */

static void
xmlParseEndTag1(xmlParserCtxtPtr ctxt, int line) {
    const xmlChar *name;

    GROW;
    if ((RAW != '<') || (NXT(1) != '/')) {
	xmlFatalErrMsg(ctxt, XML_ERR_LTSLASH_REQUIRED,
		       "xmlParseEndTag: '</' not found\n");
	return;
    }
    SKIP(2);

    name = xmlParseNameAndCompare(ctxt,ctxt->name);

    /*
     * We should definitely be at the ending "S? '>'" part
     */
    GROW;
    SKIP_BLANKS;
    if ((!IS_BYTE_CHAR(RAW)) || (RAW != '>')) {
	xmlFatalErr(ctxt, XML_ERR_GT_REQUIRED, NULL);
    } else
	NEXT1;

    /*
     * [ WFC: Element Type Match ]
     * The Name in an element's end-tag must match the element type in the
     * start-tag. 
     *
     */
    if (name != (xmlChar*)1) {
        if (name == NULL) name = BAD_CAST "unparseable";
        xmlFatalErrMsgStrIntStr(ctxt, XML_ERR_TAG_NAME_MISMATCH,
		     "Opening and ending tag mismatch: %s line %d and %s\n",
		                ctxt->name, line, name);
    }

    /*
     * SAX: End of Tag
     */
    if ((ctxt->sax != NULL) && (ctxt->sax->endElement != NULL) &&
	(!ctxt->disableSAX))
        ctxt->sax->endElement(ctxt->userData, ctxt->name);

    namePop(ctxt);
    spacePop(ctxt);
    return;
}

/**
 * xmlParseEndTag:
 * @ctxt:  an XML parser context
 *
 * parse an end of tag
 *
 * [42] ETag ::= '</' Name S? '>'
 *
 * With namespace
 *
 * [NS 9] ETag ::= '</' QName S? '>'
 */

void
xmlParseEndTag(xmlParserCtxtPtr ctxt) {
    xmlParseEndTag1(ctxt, 0);
}
#endif /* LIBXML_SAX1_ENABLED */

/************************************************************************
 *									*
 *		      SAX 2 specific operations				*
 *									*
 ************************************************************************/

static const xmlChar *
xmlParseNCNameComplex(xmlParserCtxtPtr ctxt) {
    int len = 0, l;
    int c;
    int count = 0;

    /*
     * Handler for more complex cases
     */
    GROW;
    c = CUR_CHAR(l);
    if ((c == ' ') || (c == '>') || (c == '/') || /* accelerators */
	(!IS_LETTER(c) && (c != '_'))) {
	return(NULL);
    }

    while ((c != ' ') && (c != '>') && (c != '/') && /* test bigname.xml */
	   ((IS_LETTER(c)) || (IS_DIGIT(c)) ||
            (c == '.') || (c == '-') || (c == '_') ||
	    (IS_COMBINING(c)) ||
	    (IS_EXTENDER(c)))) {
	if (count++ > 100) {
	    count = 0;
	    GROW;
	}
	len += l;
	NEXTL(l);
	c = CUR_CHAR(l);
    }
    return(xmlDictLookup(ctxt->dict, ctxt->input->cur - len, len));
}

/*
 * xmlGetNamespace:
 * @ctxt:  an XML parser context
 * @prefix:  the prefix to lookup
 *
 * Lookup the namespace name for the @prefix (which ca be NULL)
 * The prefix must come from the @ctxt->dict dictionnary
 *
 * Returns the namespace name or NULL if not bound
 */
static const xmlChar *
xmlGetNamespace(xmlParserCtxtPtr ctxt, const xmlChar *prefix) {
    int i;

    if (prefix == ctxt->str_xml) return(ctxt->str_xml_ns);
    for (i = ctxt->nsNr - 2;i >= 0;i-=2)
        if (ctxt->nsTab[i] == prefix) {
	    if ((prefix == NULL) && (*ctxt->nsTab[i + 1] == 0))
	        return(NULL);
	    return(ctxt->nsTab[i + 1]);
	}
    return(NULL);
}

/**
 * xmlParseNCName:
 * @ctxt:  an XML parser context
 *
 * parse an XML name.
 *
 * [4NS] NCNameChar ::= Letter | Digit | '.' | '-' | '_' |
 *                      CombiningChar | Extender
 *
 * [5NS] NCName ::= (Letter | '_') (NCNameChar)*
 *
 * Returns the Name parsed or NULL
 */

static const xmlChar *
xmlParseNCName(xmlParserCtxtPtr ctxt) {
    const xmlChar *in;
    const xmlChar *ret;
    int count = 0;

    /*
     * Accelerator for simple ASCII names
     */
    in = ctxt->input->cur;
    if (((*in >= 0x61) && (*in <= 0x7A)) ||
	((*in >= 0x41) && (*in <= 0x5A)) ||
	(*in == '_')) {
	in++;
	while (((*in >= 0x61) && (*in <= 0x7A)) ||
	       ((*in >= 0x41) && (*in <= 0x5A)) ||
	       ((*in >= 0x30) && (*in <= 0x39)) ||
	       (*in == '_') || (*in == '-') ||
	       (*in == '.'))
	    in++;
	if ((*in > 0) && (*in < 0x80)) {
	    count = in - ctxt->input->cur;
	    ret = xmlDictLookup(ctxt->dict, ctxt->input->cur, count);
	    ctxt->input->cur = in;
	    ctxt->nbChars += count;
	    ctxt->input->col += count;
	    if (ret == NULL) {
	        xmlErrMemory(ctxt, NULL);
	    }
	    return(ret);
	}
    }
    return(xmlParseNCNameComplex(ctxt));
}

/**
 * xmlParseQName:
 * @ctxt:  an XML parser context
 * @prefix:  pointer to store the prefix part
 *
 * parse an XML Namespace QName
 *
 * [6]  QName  ::= (Prefix ':')? LocalPart
 * [7]  Prefix  ::= NCName
 * [8]  LocalPart  ::= NCName
 *
 * Returns the Name parsed or NULL
 */

static const xmlChar *
xmlParseQName(xmlParserCtxtPtr ctxt, const xmlChar **prefix) {
    const xmlChar *l, *p;

    GROW;

    l = xmlParseNCName(ctxt);
    if (l == NULL) {
        if (CUR == ':') {
	    l = xmlParseName(ctxt);
	    if (l != NULL) {
	        xmlNsErr(ctxt, XML_NS_ERR_QNAME, 
		         "Failed to parse QName '%s'\n", l, NULL, NULL);
		*prefix = NULL;
		return(l);
	    }
	}
        return(NULL);
    }
    if (CUR == ':') {
        NEXT;
	p = l;
	l = xmlParseNCName(ctxt);
	if (l == NULL) {
	    xmlChar *tmp;

            xmlNsErr(ctxt, XML_NS_ERR_QNAME,
	             "Failed to parse QName '%s:'\n", p, NULL, NULL);
	    tmp = xmlBuildQName(BAD_CAST "", p, NULL, 0);
	    p = xmlDictLookup(ctxt->dict, tmp, -1);
	    if (tmp != NULL) xmlFree(tmp);
	    *prefix = NULL;
	    return(p);
	}
	if (CUR == ':') {
	    xmlChar *tmp;

            xmlNsErr(ctxt, XML_NS_ERR_QNAME,
	             "Failed to parse QName '%s:%s:'\n", p, l, NULL);
	    NEXT;
	    tmp = (xmlChar *) xmlParseName(ctxt);
	    if (tmp != NULL) {
	        tmp = xmlBuildQName(tmp, l, NULL, 0);
		l = xmlDictLookup(ctxt->dict, tmp, -1);
		if (tmp != NULL) xmlFree(tmp);
		*prefix = p;
		return(l);
	    }
	    tmp = xmlBuildQName(BAD_CAST "", l, NULL, 0);
	    l = xmlDictLookup(ctxt->dict, tmp, -1);
	    if (tmp != NULL) xmlFree(tmp);
	    *prefix = p;
	    return(l);
	}
	*prefix = p;
    } else
        *prefix = NULL;
    return(l);
}

/**
 * xmlParseQNameAndCompare:
 * @ctxt:  an XML parser context
 * @name:  the localname
 * @prefix:  the prefix, if any.
 *
 * parse an XML name and compares for match
 * (specialized for endtag parsing)
 *
 * Returns NULL for an illegal name, (xmlChar*) 1 for success
 * and the name for mismatch
 */

static const xmlChar *
xmlParseQNameAndCompare(xmlParserCtxtPtr ctxt, xmlChar const *name,
                        xmlChar const *prefix) {
    const xmlChar *cmp = name;
    const xmlChar *in;
    const xmlChar *ret;
    const xmlChar *prefix2;

    if (prefix == NULL) return(xmlParseNameAndCompare(ctxt, name));

    GROW;
    in = ctxt->input->cur;
    
    cmp = prefix;
    while (*in != 0 && *in == *cmp) {
    	++in;
	++cmp;
    }
    if ((*cmp == 0) && (*in == ':')) {
        in++;
	cmp = name;
	while (*in != 0 && *in == *cmp) {
	    ++in;
	    ++cmp;
	}
	if (*cmp == 0 && (*in == '>' || IS_BLANK_CH (*in))) {
	    /* success */
	    ctxt->input->cur = in;
	    return((const xmlChar*) 1);
	}
    }
    /*
     * all strings coms from the dictionary, equality can be done directly
     */
    ret = xmlParseQName (ctxt, &prefix2);
    if ((ret == name) && (prefix == prefix2))
	return((const xmlChar*) 1);
    return ret;
}

/**
 * xmlParseAttValueInternal:
 * @ctxt:  an XML parser context
 * @len:  attribute len result
 * @alloc:  whether the attribute was reallocated as a new string
 * @normalize:  if 1 then further non-CDATA normalization must be done
 *
 * parse a value for an attribute.
 * NOTE: if no normalization is needed, the routine will return pointers
 *       directly from the data buffer.
 *
 * 3.3.3 Attribute-Value Normalization:
 * Before the value of an attribute is passed to the application or
 * checked for validity, the XML processor must normalize it as follows: 
 * - a character reference is processed by appending the referenced
 *   character to the attribute value
 * - an entity reference is processed by recursively processing the
 *   replacement text of the entity 
 * - a whitespace character (#x20, #xD, #xA, #x9) is processed by
 *   appending #x20 to the normalized value, except that only a single
 *   #x20 is appended for a "#xD#xA" sequence that is part of an external
 *   parsed entity or the literal entity value of an internal parsed entity 
 * - other characters are processed by appending them to the normalized value 
 * If the declared value is not CDATA, then the XML processor must further
 * process the normalized attribute value by discarding any leading and
 * trailing space (#x20) characters, and by replacing sequences of space
 * (#x20) characters by a single space (#x20) character.  
 * All attributes for which no declaration has been read should be treated
 * by a non-validating parser as if declared CDATA.
 *
 * Returns the AttValue parsed or NULL. The value has to be freed by the
 *     caller if it was copied, this can be detected by val[*len] == 0.
 */

static xmlChar *
xmlParseAttValueInternal(xmlParserCtxtPtr ctxt, int *len, int *alloc,
                         int normalize)
{
    xmlChar limit = 0;
    const xmlChar *in = NULL, *start, *end, *last;
    xmlChar *ret = NULL;

    GROW;
    in = (xmlChar *) CUR_PTR;
    if (*in != '"' && *in != '\'') {
        xmlFatalErr(ctxt, XML_ERR_ATTRIBUTE_NOT_STARTED, NULL);
        return (NULL);
    }
    ctxt->instate = XML_PARSER_ATTRIBUTE_VALUE;

    /*
     * try to handle in this routine the most common case where no
     * allocation of a new string is required and where content is
     * pure ASCII.
     */
    limit = *in++;
    end = ctxt->input->end;
    start = in;
    if (in >= end) {
        const xmlChar *oldbase = ctxt->input->base;
	GROW;
	if (oldbase != ctxt->input->base) {
	    long delta = ctxt->input->base - oldbase;
	    start = start + delta;
	    in = in + delta;
	}
	end = ctxt->input->end;
    }
    if (normalize) {
        /*
	 * Skip any leading spaces
	 */
	while ((in < end) && (*in != limit) && 
	       ((*in == 0x20) || (*in == 0x9) ||
	        (*in == 0xA) || (*in == 0xD))) {
	    in++;
	    start = in;
	    if (in >= end) {
		const xmlChar *oldbase = ctxt->input->base;
		GROW;
		if (oldbase != ctxt->input->base) {
		    long delta = ctxt->input->base - oldbase;
		    start = start + delta;
		    in = in + delta;
		}
		end = ctxt->input->end;
	    }
	}
	while ((in < end) && (*in != limit) && (*in >= 0x20) &&
	       (*in <= 0x7f) && (*in != '&') && (*in != '<')) {
	    if ((*in++ == 0x20) && (*in == 0x20)) break;
	    if (in >= end) {
		const xmlChar *oldbase = ctxt->input->base;
		GROW;
		if (oldbase != ctxt->input->base) {
		    long delta = ctxt->input->base - oldbase;
		    start = start + delta;
		    in = in + delta;
		}
		end = ctxt->input->end;
	    }
	}
	last = in;
	/*
	 * skip the trailing blanks
	 */
	while ((last[-1] == 0x20) && (last > start)) last--;
	while ((in < end) && (*in != limit) && 
	       ((*in == 0x20) || (*in == 0x9) ||
	        (*in == 0xA) || (*in == 0xD))) {
	    in++;
	    if (in >= end) {
		const xmlChar *oldbase = ctxt->input->base;
		GROW;
		if (oldbase != ctxt->input->base) {
		    long delta = ctxt->input->base - oldbase;
		    start = start + delta;
		    in = in + delta;
		    last = last + delta;
		}
		end = ctxt->input->end;
	    }
	}
	if (*in != limit) goto need_complex;
    } else {
	while ((in < end) && (*in != limit) && (*in >= 0x20) &&
	       (*in <= 0x7f) && (*in != '&') && (*in != '<')) {
	    in++;
	    if (in >= end) {
		const xmlChar *oldbase = ctxt->input->base;
		GROW;
		if (oldbase != ctxt->input->base) {
		    long delta = ctxt->input->base - oldbase;
		    start = start + delta;
		    in = in + delta;
		}
		end = ctxt->input->end;
	    }
	}
	last = in;
	if (*in != limit) goto need_complex;
    }
    in++;
    if (len != NULL) {
        *len = last - start;
        ret = (xmlChar *) start;
    } else {
        if (alloc) *alloc = 1;
        ret = xmlStrndup(start, last - start);
    }
    CUR_PTR = in;
    if (alloc) *alloc = 0;
    return ret;
need_complex:
    if (alloc) *alloc = 1;
    return xmlParseAttValueComplex(ctxt, len, normalize);
}

/**
 * xmlParseAttribute2:
 * @ctxt:  an XML parser context
 * @pref:  the element prefix
 * @elem:  the element name
 * @prefix:  a xmlChar ** used to store the value of the attribute prefix
 * @value:  a xmlChar ** used to store the value of the attribute
 * @len:  an int * to save the length of the attribute
 * @alloc:  an int * to indicate if the attribute was allocated
 *
 * parse an attribute in the new SAX2 framework.
 *
 * Returns the attribute name, and the value in *value, .
 */

static const xmlChar *
xmlParseAttribute2(xmlParserCtxtPtr ctxt,
                   const xmlChar *pref, const xmlChar *elem,
                   const xmlChar **prefix, xmlChar **value,
		   int *len, int *alloc) {
    const xmlChar *name;
    xmlChar *val;
    int normalize = 0;

    *value = NULL;
    GROW;
    name = xmlParseQName(ctxt, prefix);
    if (name == NULL) {
	xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED,
	               "error parsing attribute name\n");
        return(NULL);
    }

    /*
     * get the type if needed
     */
    if (ctxt->attsSpecial != NULL) {
        int type;

        type = (int) (long) xmlHashQLookup2(ctxt->attsSpecial,
	                                    pref, elem, *prefix, name);
	if (type != 0) normalize = 1;
    }

    /*
     * read the value
     */
    SKIP_BLANKS;
    if (RAW == '=') {
        NEXT;
	SKIP_BLANKS;
	val = xmlParseAttValueInternal(ctxt, len, alloc, normalize);
	ctxt->instate = XML_PARSER_CONTENT;
    } else {
	xmlFatalErrMsgStr(ctxt, XML_ERR_ATTRIBUTE_WITHOUT_VALUE,
	       "Specification mandate value for attribute %s\n", name);
	return(NULL);
    }

    /*
     * Check that xml:lang conforms to the specification
     * No more registered as an error, just generate a warning now
     * since this was deprecated in XML second edition
     */
    if ((ctxt->pedantic) && (xmlStrEqual(name, BAD_CAST "xml:lang"))) {
	if (!xmlCheckLanguageID(val)) {
	    xmlWarningMsg(ctxt, XML_WAR_LANG_VALUE,
		          "Malformed value for xml:lang : %s\n",
			  val, NULL);
	}
    }

    /*
     * Check that xml:space conforms to the specification
     */
    if (xmlStrEqual(name, BAD_CAST "xml:space")) {
	if (xmlStrEqual(val, BAD_CAST "default"))
	    *(ctxt->space) = 0;
	else if (xmlStrEqual(val, BAD_CAST "preserve"))
	    *(ctxt->space) = 1;
	else {
	    xmlFatalErrMsgStr(ctxt, XML_ERR_ATTRIBUTE_WITHOUT_VALUE,
"Invalid value \"%s\" for xml:space : \"default\" or \"preserve\" expected\n",
                                 val);
	}
    }

    *value = val;
    return(name);
}

/**
 * xmlParseStartTag2:
 * @ctxt:  an XML parser context
 * 
 * parse a start of tag either for rule element or
 * EmptyElement. In both case we don't parse the tag closing chars.
 * This routine is called when running SAX2 parsing
 *
 * [40] STag ::= '<' Name (S Attribute)* S? '>'
 *
 * [ WFC: Unique Att Spec ]
 * No attribute name may appear more than once in the same start-tag or
 * empty-element tag. 
 *
 * [44] EmptyElemTag ::= '<' Name (S Attribute)* S? '/>'
 *
 * [ WFC: Unique Att Spec ]
 * No attribute name may appear more than once in the same start-tag or
 * empty-element tag. 
 *
 * With namespace:
 *
 * [NS 8] STag ::= '<' QName (S Attribute)* S? '>'
 *
 * [NS 10] EmptyElement ::= '<' QName (S Attribute)* S? '/>'
 *
 * Returns the element name parsed
 */

static const xmlChar *
xmlParseStartTag2(xmlParserCtxtPtr ctxt, const xmlChar **pref,
                  const xmlChar **URI) {
    const xmlChar *localname;
    const xmlChar *prefix;
    const xmlChar *attname;
    const xmlChar *aprefix;
    const xmlChar *nsname;
    xmlChar *attvalue;
    const xmlChar **atts = ctxt->atts;
    int maxatts = ctxt->maxatts;
    int nratts, nbatts, nbdef;
    int i, j, nbNs, attval;
    const xmlChar *base;
    unsigned long cur;

    if (RAW != '<') return(NULL);
    NEXT1;

    /*
     * NOTE: it is crucial with the SAX2 API to never call SHRINK beyond that
     *       point since the attribute values may be stored as pointers to
     *       the buffer and calling SHRINK would destroy them !
     *       The Shrinking is only possible once the full set of attribute
     *       callbacks have been done.
     */
reparse:
    SHRINK;
    base = ctxt->input->base;
    cur = ctxt->input->cur - ctxt->input->base;
    nbatts = 0;
    nratts = 0;
    nbdef = 0;
    nbNs = 0;
    attval = 0;

    localname = xmlParseQName(ctxt, &prefix);
    if (localname == NULL) {
	xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED,
		       "StartTag: invalid element name\n");
        return(NULL);
    }

    /*
     * Now parse the attributes, it ends up with the ending
     *
     * (S Attribute)* S?
     */
    SKIP_BLANKS;
    GROW;
    if (ctxt->input->base != base) goto base_changed;

    while ((RAW != '>') && 
	   ((RAW != '/') || (NXT(1) != '>')) &&
	   (IS_BYTE_CHAR(RAW))) {
	const xmlChar *q = CUR_PTR;
	unsigned int cons = ctxt->input->consumed;
	int len = -1, alloc = 0;

	attname = xmlParseAttribute2(ctxt, prefix, localname,
	                             &aprefix, &attvalue, &len, &alloc);
        if ((attname != NULL) && (attvalue != NULL)) {
	    if (len < 0) len = xmlStrlen(attvalue);
            if ((attname == ctxt->str_xmlns) && (aprefix == NULL)) {
	        const xmlChar *URL = xmlDictLookup(ctxt->dict, attvalue, len);
		xmlURIPtr uri;

                if (*URL != 0) {
		    uri = xmlParseURI((const char *) URL);
		    if (uri == NULL) {
			xmlWarningMsg(ctxt, XML_WAR_NS_URI,
				      "xmlns: %s not a valid URI\n",
				      URL, NULL);
		    } else {
			if (uri->scheme == NULL) {
			    xmlWarningMsg(ctxt, XML_WAR_NS_URI_RELATIVE,
				   "xmlns: URI %s is not absolute\n",
				          URL, NULL);
			}
			xmlFreeURI(uri);
		    }
		}
		/*
		 * check that it's not a defined namespace
		 */
		for (j = 1;j <= nbNs;j++)
		    if (ctxt->nsTab[ctxt->nsNr - 2 * j] == NULL)
			break;
		if (j <= nbNs)
		    xmlErrAttributeDup(ctxt, NULL, attname);
		else
		    if (nsPush(ctxt, NULL, URL) > 0) nbNs++;
		if (alloc != 0) xmlFree(attvalue);
		SKIP_BLANKS;
		continue;
	    }
            if (aprefix == ctxt->str_xmlns) {
	        const xmlChar *URL = xmlDictLookup(ctxt->dict, attvalue, len);
		xmlURIPtr uri;

                if (attname == ctxt->str_xml) {
		    if (URL != ctxt->str_xml_ns) {
		        xmlNsErr(ctxt, XML_NS_ERR_XML_NAMESPACE,
			         "xml namespace prefix mapped to wrong URI\n",
			         NULL, NULL, NULL);
		    }
		    /*
		     * Do not keep a namespace definition node
		     */
		    if (alloc != 0) xmlFree(attvalue);
		    SKIP_BLANKS;
		    continue;
		}
		uri = xmlParseURI((const char *) URL);
		if (uri == NULL) {
		    xmlWarningMsg(ctxt, XML_WAR_NS_URI,
			 "xmlns:%s: '%s' is not a valid URI\n",
				       attname, URL);
		} else {
		    if ((ctxt->pedantic) && (uri->scheme == NULL)) {
			xmlWarningMsg(ctxt, XML_WAR_NS_URI_RELATIVE,
				      "xmlns:%s: URI %s is not absolute\n",
			              attname, URL);
		    }
		    xmlFreeURI(uri);
		}

		/*
		 * check that it's not a defined namespace
		 */
		for (j = 1;j <= nbNs;j++)
		    if (ctxt->nsTab[ctxt->nsNr - 2 * j] == attname)
			break;
		if (j <= nbNs)
		    xmlErrAttributeDup(ctxt, aprefix, attname);
		else
		    if (nsPush(ctxt, attname, URL) > 0) nbNs++;
		if (alloc != 0) xmlFree(attvalue);
		SKIP_BLANKS;
		continue;
	    }

	    /*
	     * Add the pair to atts
	     */
	    if ((atts == NULL) || (nbatts + 5 > maxatts)) {
	        if (xmlCtxtGrowAttrs(ctxt, nbatts + 5) < 0) {
		    if (attvalue[len] == 0)
			xmlFree(attvalue);
		    goto failed;
		}
	        maxatts = ctxt->maxatts;
		atts = ctxt->atts;
	    }
	    ctxt->attallocs[nratts++] = alloc;
	    atts[nbatts++] = attname;
	    atts[nbatts++] = aprefix;
	    atts[nbatts++] = NULL; /* the URI will be fetched later */
	    atts[nbatts++] = attvalue;
	    attvalue += len;
	    atts[nbatts++] = attvalue;
	    /*
	     * tag if some deallocation is needed
	     */
	    if (alloc != 0) attval = 1;
	} else {
	    if ((attvalue != NULL) && (attvalue[len] == 0))
		xmlFree(attvalue);
	}

failed:     

	GROW
	if (ctxt->input->base != base) goto base_changed;
	if ((RAW == '>') || (((RAW == '/') && (NXT(1) == '>'))))
	    break;
	if (!IS_BLANK_CH(RAW)) {
	    xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
			   "attributes construct error\n");
	}
	SKIP_BLANKS;
        if ((cons == ctxt->input->consumed) && (q == CUR_PTR) &&
            (attname == NULL) && (attvalue == NULL)) {
	    xmlFatalErr(ctxt, XML_ERR_INTERNAL_ERROR,
	         "xmlParseStartTag: problem parsing attributes\n");
	    break;
	}
        GROW;
	if (ctxt->input->base != base) goto base_changed;
    }

    /*
     * The attributes checkings
     */
    for (i = 0; i < nbatts;i += 5) {
        nsname = xmlGetNamespace(ctxt, atts[i + 1]);
	if ((atts[i + 1] != NULL) && (nsname == NULL)) {
	    xmlNsErr(ctxt, XML_NS_ERR_UNDEFINED_NAMESPACE,
		 "Namespace prefix %s for %s on %s is not defined\n",
		     atts[i + 1], atts[i], localname);
	}
	atts[i + 2] = nsname;
	/*
	 * [ WFC: Unique Att Spec ]
	 * No attribute name may appear more than once in the same
	 * start-tag or empty-element tag. 
	 * As extended by the Namespace in XML REC.
	 */
        for (j = 0; j < i;j += 5) {
	    if (atts[i] == atts[j]) {
	        if (atts[i+1] == atts[j+1]) {
		    xmlErrAttributeDup(ctxt, atts[i+1], atts[i]);
		    break;
		}
		if ((nsname != NULL) && (atts[j + 2] == nsname)) {
		    xmlNsErr(ctxt, XML_NS_ERR_ATTRIBUTE_REDEFINED,
			     "Namespaced Attribute %s in '%s' redefined\n",
			     atts[i], nsname, NULL);
		    break;
		}
	    }
	}
    }

    /*
     * The attributes defaulting
     */
    if (ctxt->attsDefault != NULL) {
        xmlDefAttrsPtr defaults;

	defaults = xmlHashLookup2(ctxt->attsDefault, localname, prefix);
	if (defaults != NULL) {
	    for (i = 0;i < defaults->nbAttrs;i++) {
	        attname = defaults->values[4 * i];
		aprefix = defaults->values[4 * i + 1];

                /*
		 * special work for namespaces defaulted defs
		 */
		if ((attname == ctxt->str_xmlns) && (aprefix == NULL)) {
		    /*
		     * check that it's not a defined namespace
		     */
		    for (j = 1;j <= nbNs;j++)
		        if (ctxt->nsTab[ctxt->nsNr - 2 * j] == NULL)
			    break;
	            if (j <= nbNs) continue;

		    nsname = xmlGetNamespace(ctxt, NULL);
		    if (nsname != defaults->values[4 * i + 2]) {
			if (nsPush(ctxt, NULL,
			           defaults->values[4 * i + 2]) > 0)
			    nbNs++;
		    }
		} else if (aprefix == ctxt->str_xmlns) {
		    /*
		     * check that it's not a defined namespace
		     */
		    for (j = 1;j <= nbNs;j++)
		        if (ctxt->nsTab[ctxt->nsNr - 2 * j] == attname)
			    break;
	            if (j <= nbNs) continue;

		    nsname = xmlGetNamespace(ctxt, attname);
		    if (nsname != defaults->values[2]) {
			if (nsPush(ctxt, attname,
			           defaults->values[4 * i + 2]) > 0)
			    nbNs++;
		    }
		} else {
		    /*
		     * check that it's not a defined attribute
		     */
		    for (j = 0;j < nbatts;j+=5) {
			if ((attname == atts[j]) && (aprefix == atts[j+1]))
			    break;
		    }
		    if (j < nbatts) continue;

		    if ((atts == NULL) || (nbatts + 5 > maxatts)) {
			if (xmlCtxtGrowAttrs(ctxt, nbatts + 5) < 0) {
			    return(NULL);
			}
			maxatts = ctxt->maxatts;
			atts = ctxt->atts;
		    }
		    atts[nbatts++] = attname;
		    atts[nbatts++] = aprefix;
		    if (aprefix == NULL)
			atts[nbatts++] = NULL;
		    else
		        atts[nbatts++] = xmlGetNamespace(ctxt, aprefix);
		    atts[nbatts++] = defaults->values[4 * i + 2];
		    atts[nbatts++] = defaults->values[4 * i + 3];
		    nbdef++;
		}
	    }
	}
    }

    nsname = xmlGetNamespace(ctxt, prefix);
    if ((prefix != NULL) && (nsname == NULL)) {
	xmlNsErr(ctxt, XML_NS_ERR_UNDEFINED_NAMESPACE,
	         "Namespace prefix %s on %s is not defined\n",
		 prefix, localname, NULL);
    }
    *pref = prefix;
    *URI = nsname;

    /*
     * SAX: Start of Element !
     */
    if ((ctxt->sax != NULL) && (ctxt->sax->startElementNs != NULL) &&
	(!ctxt->disableSAX)) {
	if (nbNs > 0)
	    ctxt->sax->startElementNs(ctxt->userData, localname, prefix,
			  nsname, nbNs, &ctxt->nsTab[ctxt->nsNr - 2 * nbNs],
			  nbatts / 5, nbdef, atts);
	else
	    ctxt->sax->startElementNs(ctxt->userData, localname, prefix,
	                  nsname, 0, NULL, nbatts / 5, nbdef, atts);
    }

    /*
     * Free up attribute allocated strings if needed
     */
    if (attval != 0) {
	for (i = 3,j = 0; j < nratts;i += 5,j++)
	    if ((ctxt->attallocs[j] != 0) && (atts[i] != NULL))
	        xmlFree((xmlChar *) atts[i]);
    }

    return(localname);

base_changed:
    /*
     * the attribute strings are valid iif the base didn't changed
     */
    if (attval != 0) {
	for (i = 3,j = 0; j < nratts;i += 5,j++)
	    if ((ctxt->attallocs[j] != 0) && (atts[i] != NULL))
	        xmlFree((xmlChar *) atts[i]);
    }
    ctxt->input->cur = ctxt->input->base + cur;
    if (ctxt->wellFormed == 1) {
	goto reparse;
    }
    return(NULL);
}

/**
 * xmlParseEndTag2:
 * @ctxt:  an XML parser context
 * @line:  line of the start tag
 * @nsNr:  number of namespaces on the start tag
 *
 * parse an end of tag
 *
 * [42] ETag ::= '</' Name S? '>'
 *
 * With namespace
 *
 * [NS 9] ETag ::= '</' QName S? '>'
 */

static void
xmlParseEndTag2(xmlParserCtxtPtr ctxt, const xmlChar *prefix,
                const xmlChar *URI, int line, int nsNr) {
    const xmlChar *name;

    GROW;
    if ((RAW != '<') || (NXT(1) != '/')) {
	xmlFatalErr(ctxt, XML_ERR_LTSLASH_REQUIRED, NULL);
	return;
    }
    SKIP(2);

    name = xmlParseQNameAndCompare(ctxt, ctxt->name, prefix);

    /*
     * We should definitely be at the ending "S? '>'" part
     */
    GROW;
    SKIP_BLANKS;
    if ((!IS_BYTE_CHAR(RAW)) || (RAW != '>')) {
	xmlFatalErr(ctxt, XML_ERR_GT_REQUIRED, NULL);
    } else
	NEXT1;

    /*
     * [ WFC: Element Type Match ]
     * The Name in an element's end-tag must match the element type in the
     * start-tag. 
     *
     */
    if (name != (xmlChar*)1) {
        if (name == NULL) name = BAD_CAST "unparseable";
        xmlFatalErrMsgStrIntStr(ctxt, XML_ERR_TAG_NAME_MISMATCH,
		     "Opening and ending tag mismatch: %s line %d and %s\n",
		                ctxt->name, line, name);
    }

    /*
     * SAX: End of Tag
     */
    if ((ctxt->sax != NULL) && (ctxt->sax->endElementNs != NULL) &&
	(!ctxt->disableSAX))
	ctxt->sax->endElementNs(ctxt->userData, ctxt->name, prefix, URI);

    spacePop(ctxt);
    if (nsNr != 0)
	nsPop(ctxt, nsNr);
    return;
}

/**
 * xmlParseCDSect:
 * @ctxt:  an XML parser context
 * 
 * Parse escaped pure raw content.
 *
 * [18] CDSect ::= CDStart CData CDEnd
 *
 * [19] CDStart ::= '<![CDATA['
 *
 * [20] Data ::= (Char* - (Char* ']]>' Char*))
 *
 * [21] CDEnd ::= ']]>'
 */
void
xmlParseCDSect(xmlParserCtxtPtr ctxt) {
    xmlChar *buf = NULL;
    int len = 0;
    int size = XML_PARSER_BUFFER_SIZE;
    int r, rl;
    int	s, sl;
    int cur, l;
    int count = 0;

    /* Check 2.6.0 was NXT(0) not RAW */
    if (CMP9(CUR_PTR, '<', '!', '[', 'C', 'D', 'A', 'T', 'A', '[')) {
	SKIP(9);
    } else
        return;

    ctxt->instate = XML_PARSER_CDATA_SECTION;
    r = CUR_CHAR(rl);
    if (!IS_CHAR(r)) {
	xmlFatalErr(ctxt, XML_ERR_CDATA_NOT_FINISHED, NULL);
	ctxt->instate = XML_PARSER_CONTENT;
        return;
    }
    NEXTL(rl);
    s = CUR_CHAR(sl);
    if (!IS_CHAR(s)) {
	xmlFatalErr(ctxt, XML_ERR_CDATA_NOT_FINISHED, NULL);
	ctxt->instate = XML_PARSER_CONTENT;
        return;
    }
    NEXTL(sl);
    cur = CUR_CHAR(l);
    buf = (xmlChar *) xmlMallocAtomic(size * sizeof(xmlChar));
    if (buf == NULL) {
	xmlErrMemory(ctxt, NULL);
	return;
    }
    while (IS_CHAR(cur) &&
           ((r != ']') || (s != ']') || (cur != '>'))) {
	if (len + 5 >= size) {
	    size *= 2;
	    buf = (xmlChar *) xmlRealloc(buf, size * sizeof(xmlChar));
	    if (buf == NULL) {
		xmlErrMemory(ctxt, NULL);
		return;
	    }
	}
	COPY_BUF(rl,buf,len,r);
	r = s;
	rl = sl;
	s = cur;
	sl = l;
	count++;
	if (count > 50) {
	    GROW;
	    count = 0;
	}
	NEXTL(l);
	cur = CUR_CHAR(l);
    }
    buf[len] = 0;
    ctxt->instate = XML_PARSER_CONTENT;
    if (cur != '>') {
	xmlFatalErrMsgStr(ctxt, XML_ERR_CDATA_NOT_FINISHED,
	                     "CData section not finished\n%.50s\n", buf);
	xmlFree(buf);
        return;
    }
    NEXTL(l);

    /*
     * OK the buffer is to be consumed as cdata.
     */
    if ((ctxt->sax != NULL) && (!ctxt->disableSAX)) {
	if (ctxt->sax->cdataBlock != NULL)
	    ctxt->sax->cdataBlock(ctxt->userData, buf, len);
	else if (ctxt->sax->characters != NULL)
	    ctxt->sax->characters(ctxt->userData, buf, len);
    }
    xmlFree(buf);
}

/**
 * xmlParseContent:
 * @ctxt:  an XML parser context
 *
 * Parse a content:
 *
 * [43] content ::= (element | CharData | Reference | CDSect | PI | Comment)*
 */

void
xmlParseContent(xmlParserCtxtPtr ctxt) {
    GROW;
    while ((RAW != 0) &&
	   ((RAW != '<') || (NXT(1) != '/'))) {
	const xmlChar *test = CUR_PTR;
	unsigned int cons = ctxt->input->consumed;
	const xmlChar *cur = ctxt->input->cur;

	/*
	 * First case : a Processing Instruction.
	 */
	if ((*cur == '<') && (cur[1] == '?')) {
	    xmlParsePI(ctxt);
	}

	/*
	 * Second case : a CDSection
	 */
	/* 2.6.0 test was *cur not RAW */
	else if (CMP9(CUR_PTR, '<', '!', '[', 'C', 'D', 'A', 'T', 'A', '[')) {
	    xmlParseCDSect(ctxt);
	}

	/*
	 * Third case :  a comment
	 */
	else if ((*cur == '<') && (NXT(1) == '!') &&
		 (NXT(2) == '-') && (NXT(3) == '-')) {
	    xmlParseComment(ctxt);
	    ctxt->instate = XML_PARSER_CONTENT;
	}

	/*
	 * Fourth case :  a sub-element.
	 */
	else if (*cur == '<') {
	    xmlParseElement(ctxt);
	}

	/*
	 * Fifth case : a reference. If if has not been resolved,
	 *    parsing returns it's Name, create the node 
	 */

	else if (*cur == '&') {
	    xmlParseReference(ctxt);
	}

	/*
	 * Last case, text. Note that References are handled directly.
	 */
	else {
	    xmlParseCharData(ctxt, 0);
	}

	GROW;
	/*
	 * Pop-up of finished entities.
	 */
	while ((RAW == 0) && (ctxt->inputNr > 1))
	    xmlPopInput(ctxt);
	SHRINK;

	if ((cons == ctxt->input->consumed) && (test == CUR_PTR)) {
	    xmlFatalErr(ctxt, XML_ERR_INTERNAL_ERROR,
	                "detected an error in element content\n");
	    ctxt->instate = XML_PARSER_EOF;
            break;
	}
    }
}

/**
 * xmlParseElement:
 * @ctxt:  an XML parser context
 *
 * parse an XML element, this is highly recursive
 *
 * [39] element ::= EmptyElemTag | STag content ETag
 *
 * [ WFC: Element Type Match ]
 * The Name in an element's end-tag must match the element type in the
 * start-tag. 
 *
 */

void
xmlParseElement(xmlParserCtxtPtr ctxt) {
    const xmlChar *name;
    const xmlChar *prefix;
    const xmlChar *URI;
    xmlParserNodeInfo node_info;
    int line;
    xmlNodePtr ret;
    int nsNr = ctxt->nsNr;

    /* Capture start position */
    if (ctxt->record_info) {
        node_info.begin_pos = ctxt->input->consumed +
                          (CUR_PTR - ctxt->input->base);
	node_info.begin_line = ctxt->input->line;
    }

    if (ctxt->spaceNr == 0)
	spacePush(ctxt, -1);
    else
	spacePush(ctxt, *ctxt->space);

    line = ctxt->input->line;
#ifdef LIBXML_SAX1_ENABLED
    if (ctxt->sax2)
#endif /* LIBXML_SAX1_ENABLED */
        name = xmlParseStartTag2(ctxt, &prefix, &URI);
#ifdef LIBXML_SAX1_ENABLED
    else
	name = xmlParseStartTag(ctxt);
#endif /* LIBXML_SAX1_ENABLED */
    if (name == NULL) {
	spacePop(ctxt);
        return;
    }
    namePush(ctxt, name);
    ret = ctxt->node;

#ifdef LIBXML_VALID_ENABLED
    /*
     * [ VC: Root Element Type ]
     * The Name in the document type declaration must match the element
     * type of the root element. 
     */
    if (ctxt->validate && ctxt->wellFormed && ctxt->myDoc &&
        ctxt->node && (ctxt->node == ctxt->myDoc->children))
        ctxt->valid &= xmlValidateRoot(&ctxt->vctxt, ctxt->myDoc);
#endif /* LIBXML_VALID_ENABLED */

    /*
     * Check for an Empty Element.
     */
    if ((RAW == '/') && (NXT(1) == '>')) {
        SKIP(2);
	if (ctxt->sax2) {
	    if ((ctxt->sax != NULL) && (ctxt->sax->endElementNs != NULL) &&
		(!ctxt->disableSAX))
		ctxt->sax->endElementNs(ctxt->userData, name, prefix, URI);
#ifdef LIBXML_SAX1_ENABLED
	} else {
	    if ((ctxt->sax != NULL) && (ctxt->sax->endElement != NULL) &&
		(!ctxt->disableSAX))
		ctxt->sax->endElement(ctxt->userData, name);
#endif /* LIBXML_SAX1_ENABLED */
	}
	namePop(ctxt);
	spacePop(ctxt);
	if (nsNr != ctxt->nsNr)
	    nsPop(ctxt, ctxt->nsNr - nsNr);
	if ( ret != NULL && ctxt->record_info ) {
	   node_info.end_pos = ctxt->input->consumed +
			      (CUR_PTR - ctxt->input->base);
	   node_info.end_line = ctxt->input->line;
	   node_info.node = ret;
	   xmlParserAddNodeInfo(ctxt, &node_info);
	}
	return;
    }
    if (RAW == '>') {
        NEXT1;
    } else {
        xmlFatalErrMsgStrIntStr(ctxt, XML_ERR_GT_REQUIRED,
		     "Couldn't find end of Start Tag %s line %d\n",
		                name, line, NULL);

	/*
	 * end of parsing of this node.
	 */
	nodePop(ctxt);
	namePop(ctxt);
	spacePop(ctxt);
	if (nsNr != ctxt->nsNr)
	    nsPop(ctxt, ctxt->nsNr - nsNr);

	/*
	 * Capture end position and add node
	 */
	if ( ret != NULL && ctxt->record_info ) {
	   node_info.end_pos = ctxt->input->consumed +
			      (CUR_PTR - ctxt->input->base);
	   node_info.end_line = ctxt->input->line;
	   node_info.node = ret;
	   xmlParserAddNodeInfo(ctxt, &node_info);
	}
	return;
    }

    /*
     * Parse the content of the element:
     */
    xmlParseContent(ctxt);
    if (!IS_BYTE_CHAR(RAW)) {
        xmlFatalErrMsgStrIntStr(ctxt, XML_ERR_TAG_NOT_FINISHED,
	 "Premature end of data in tag %s line %d\n",
		                name, line, NULL);

	/*
	 * end of parsing of this node.
	 */
	nodePop(ctxt);
	namePop(ctxt);
	spacePop(ctxt);
	if (nsNr != ctxt->nsNr)
	    nsPop(ctxt, ctxt->nsNr - nsNr);
	return;
    }

    /*
     * parse the end of tag: '</' should be here.
     */
    if (ctxt->sax2) {
	xmlParseEndTag2(ctxt, prefix, URI, line, ctxt->nsNr - nsNr);
	namePop(ctxt);
    }
#ifdef LIBXML_SAX1_ENABLED
      else
	xmlParseEndTag1(ctxt, line);
#endif /* LIBXML_SAX1_ENABLED */

    /*
     * Capture end position and add node
     */
    if ( ret != NULL && ctxt->record_info ) {
       node_info.end_pos = ctxt->input->consumed +
                          (CUR_PTR - ctxt->input->base);
       node_info.end_line = ctxt->input->line;
       node_info.node = ret;
       xmlParserAddNodeInfo(ctxt, &node_info);
    }
}

/**
 * xmlParseVersionNum:
 * @ctxt:  an XML parser context
 *
 * parse the XML version value.
 *
 * [26] VersionNum ::= ([a-zA-Z0-9_.:] | '-')+
 *
 * Returns the string giving the XML version number, or NULL
 */
xmlChar *
xmlParseVersionNum(xmlParserCtxtPtr ctxt) {
    xmlChar *buf = NULL;
    int len = 0;
    int size = 10;
    xmlChar cur;

    buf = (xmlChar *) xmlMallocAtomic(size * sizeof(xmlChar));
    if (buf == NULL) {
	xmlErrMemory(ctxt, NULL);
	return(NULL);
    }
    cur = CUR;
    while (((cur >= 'a') && (cur <= 'z')) ||
           ((cur >= 'A') && (cur <= 'Z')) ||
           ((cur >= '0') && (cur <= '9')) ||
           (cur == '_') || (cur == '.') ||
	   (cur == ':') || (cur == '-')) {
	if (len + 1 >= size) {
	    size *= 2;
	    buf = (xmlChar *) xmlRealloc(buf, size * sizeof(xmlChar));
	    if (buf == NULL) {
		xmlErrMemory(ctxt, NULL);
		return(NULL);
	    }
	}
	buf[len++] = cur;
	NEXT;
	cur=CUR;
    }
    buf[len] = 0;
    return(buf);
}

/**
 * xmlParseVersionInfo:
 * @ctxt:  an XML parser context
 * 
 * parse the XML version.
 *
 * [24] VersionInfo ::= S 'version' Eq (' VersionNum ' | " VersionNum ")
 * 
 * [25] Eq ::= S? '=' S?
 *
 * Returns the version string, e.g. "1.0"
 */

xmlChar *
xmlParseVersionInfo(xmlParserCtxtPtr ctxt) {
    xmlChar *version = NULL;

    if (CMP7(CUR_PTR, 'v', 'e', 'r', 's', 'i', 'o', 'n')) {
	SKIP(7);
	SKIP_BLANKS;
	if (RAW != '=') {
	    xmlFatalErr(ctxt, XML_ERR_EQUAL_REQUIRED, NULL);
	    return(NULL);
        }
	NEXT;
	SKIP_BLANKS;
	if (RAW == '"') {
	    NEXT;
	    version = xmlParseVersionNum(ctxt);
	    if (RAW != '"') {
		xmlFatalErr(ctxt, XML_ERR_STRING_NOT_CLOSED, NULL);
	    } else
	        NEXT;
	} else if (RAW == '\''){
	    NEXT;
	    version = xmlParseVersionNum(ctxt);
	    if (RAW != '\'') {
		xmlFatalErr(ctxt, XML_ERR_STRING_NOT_CLOSED, NULL);
	    } else
	        NEXT;
	} else {
	    xmlFatalErr(ctxt, XML_ERR_STRING_NOT_STARTED, NULL);
	}
    }
    return(version);
}

/**
 * xmlParseEncName:
 * @ctxt:  an XML parser context
 *
 * parse the XML encoding name
 *
 * [81] EncName ::= [A-Za-z] ([A-Za-z0-9._] | '-')*
 *
 * Returns the encoding name value or NULL
 */
xmlChar *
xmlParseEncName(xmlParserCtxtPtr ctxt) {
    xmlChar *buf = NULL;
    int len = 0;
    int size = 10;
    xmlChar cur;

    cur = CUR;
    if (((cur >= 'a') && (cur <= 'z')) ||
        ((cur >= 'A') && (cur <= 'Z'))) {
	buf = (xmlChar *) xmlMallocAtomic(size * sizeof(xmlChar));
	if (buf == NULL) {
	    xmlErrMemory(ctxt, NULL);
	    return(NULL);
	}
	
	buf[len++] = cur;
	NEXT;
	cur = CUR;
	while (((cur >= 'a') && (cur <= 'z')) ||
	       ((cur >= 'A') && (cur <= 'Z')) ||
	       ((cur >= '0') && (cur <= '9')) ||
	       (cur == '.') || (cur == '_') ||
	       (cur == '-')) {
	    if (len + 1 >= size) {
		size *= 2;
		buf = (xmlChar *) xmlRealloc(buf, size * sizeof(xmlChar));
		if (buf == NULL) {
		    xmlErrMemory(ctxt, NULL);
		    return(NULL);
		}
	    }
	    buf[len++] = cur;
	    NEXT;
	    cur = CUR;
	    if (cur == 0) {
	        SHRINK;
		GROW;
		cur = CUR;
	    }
        }
	buf[len] = 0;
    } else {
	xmlFatalErr(ctxt, XML_ERR_ENCODING_NAME, NULL);
    }
    return(buf);
}

/**
 * xmlParseEncodingDecl:
 * @ctxt:  an XML parser context
 * 
 * parse the XML encoding declaration
 *
 * [80] EncodingDecl ::= S 'encoding' Eq ('"' EncName '"' |  "'" EncName "'")
 *
 * this setups the conversion filters.
 *
 * Returns the encoding value or NULL
 */

const xmlChar *
xmlParseEncodingDecl(xmlParserCtxtPtr ctxt) {
    xmlChar *encoding = NULL;

    SKIP_BLANKS;
    if (CMP8(CUR_PTR, 'e', 'n', 'c', 'o', 'd', 'i', 'n', 'g')) {
	SKIP(8);
	SKIP_BLANKS;
	if (RAW != '=') {
	    xmlFatalErr(ctxt, XML_ERR_EQUAL_REQUIRED, NULL);
	    return(NULL);
        }
	NEXT;
	SKIP_BLANKS;
	if (RAW == '"') {
	    NEXT;
	    encoding = xmlParseEncName(ctxt);
	    if (RAW != '"') {
		xmlFatalErr(ctxt, XML_ERR_STRING_NOT_CLOSED, NULL);
	    } else
	        NEXT;
	} else if (RAW == '\''){
	    NEXT;
	    encoding = xmlParseEncName(ctxt);
	    if (RAW != '\'') {
		xmlFatalErr(ctxt, XML_ERR_STRING_NOT_CLOSED, NULL);
	    } else
	        NEXT;
	} else {
	    xmlFatalErr(ctxt, XML_ERR_STRING_NOT_STARTED, NULL);
	}
	/*
	 * UTF-16 encoding stwich has already taken place at this stage,
	 * more over the little-endian/big-endian selection is already done
	 */
        if ((encoding != NULL) &&
	    ((!xmlStrcasecmp(encoding, BAD_CAST "UTF-16")) ||
	     (!xmlStrcasecmp(encoding, BAD_CAST "UTF16")))) {
	    if (ctxt->encoding != NULL)
		xmlFree((xmlChar *) ctxt->encoding);
	    ctxt->encoding = encoding;
	}
	/*
	 * UTF-8 encoding is handled natively
	 */
        else if ((encoding != NULL) &&
	    ((!xmlStrcasecmp(encoding, BAD_CAST "UTF-8")) ||
	     (!xmlStrcasecmp(encoding, BAD_CAST "UTF8")))) {
	    if (ctxt->encoding != NULL)
		xmlFree((xmlChar *) ctxt->encoding);
	    ctxt->encoding = encoding;
	}
	else if (encoding != NULL) {
	    xmlCharEncodingHandlerPtr handler;

	    if (ctxt->input->encoding != NULL)
		xmlFree((xmlChar *) ctxt->input->encoding);
	    ctxt->input->encoding = encoding;

            handler = xmlFindCharEncodingHandler((const char *) encoding);
	    if (handler != NULL) {
		xmlSwitchToEncoding(ctxt, handler);
	    } else {
		xmlFatalErrMsgStr(ctxt, XML_ERR_UNSUPPORTED_ENCODING,
			"Unsupported encoding %s\n", encoding);
		return(NULL);
	    }
	}
    }
    return(encoding);
}

/**
 * xmlParseSDDecl:
 * @ctxt:  an XML parser context
 *
 * parse the XML standalone declaration
 *
 * [32] SDDecl ::= S 'standalone' Eq
 *                 (("'" ('yes' | 'no') "'") | ('"' ('yes' | 'no')'"')) 
 *
 * [ VC: Standalone Document Declaration ]
 * TODO The standalone document declaration must have the value "no"
 * if any external markup declarations contain declarations of:
 *  - attributes with default values, if elements to which these
 *    attributes apply appear in the document without specifications
 *    of values for these attributes, or
 *  - entities (other than amp, lt, gt, apos, quot), if references
 *    to those entities appear in the document, or
 *  - attributes with values subject to normalization, where the
 *    attribute appears in the document with a value which will change
 *    as a result of normalization, or
 *  - element types with element content, if white space occurs directly
 *    within any instance of those types.
 *
 * Returns 1 if standalone, 0 otherwise
 */

int
xmlParseSDDecl(xmlParserCtxtPtr ctxt) {
    int standalone = -1;

    SKIP_BLANKS;
    if (CMP10(CUR_PTR, 's', 't', 'a', 'n', 'd', 'a', 'l', 'o', 'n', 'e')) {
	SKIP(10);
        SKIP_BLANKS;
	if (RAW != '=') {
	    xmlFatalErr(ctxt, XML_ERR_EQUAL_REQUIRED, NULL);
	    return(standalone);
        }
	NEXT;
	SKIP_BLANKS;
        if (RAW == '\''){
	    NEXT;
	    if ((RAW == 'n') && (NXT(1) == 'o')) {
	        standalone = 0;
                SKIP(2);
	    } else if ((RAW == 'y') && (NXT(1) == 'e') &&
	               (NXT(2) == 's')) {
	        standalone = 1;
		SKIP(3);
            } else {
		xmlFatalErr(ctxt, XML_ERR_STANDALONE_VALUE, NULL);
	    }
	    if (RAW != '\'') {
		xmlFatalErr(ctxt, XML_ERR_STRING_NOT_CLOSED, NULL);
	    } else
	        NEXT;
	} else if (RAW == '"'){
	    NEXT;
	    if ((RAW == 'n') && (NXT(1) == 'o')) {
	        standalone = 0;
		SKIP(2);
	    } else if ((RAW == 'y') && (NXT(1) == 'e') &&
	               (NXT(2) == 's')) {
	        standalone = 1;
                SKIP(3);
            } else {
		xmlFatalErr(ctxt, XML_ERR_STANDALONE_VALUE, NULL);
	    }
	    if (RAW != '"') {
		xmlFatalErr(ctxt, XML_ERR_STRING_NOT_CLOSED, NULL);
	    } else
	        NEXT;
	} else {
	    xmlFatalErr(ctxt, XML_ERR_STRING_NOT_STARTED, NULL);
        }
    }
    return(standalone);
}

/**
 * xmlParseXMLDecl:
 * @ctxt:  an XML parser context
 * 
 * parse an XML declaration header
 *
 * [23] XMLDecl ::= '<?xml' VersionInfo EncodingDecl? SDDecl? S? '?>'
 */

void
xmlParseXMLDecl(xmlParserCtxtPtr ctxt) {
    xmlChar *version;

    /*
     * We know that '<?xml' is here.
     */
    SKIP(5);

    if (!IS_BLANK_CH(RAW)) {
	xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED,
	               "Blank needed after '<?xml'\n");
    }
    SKIP_BLANKS;

    /*
     * We must have the VersionInfo here.
     */
    version = xmlParseVersionInfo(ctxt);
    if (version == NULL) {
	xmlFatalErr(ctxt, XML_ERR_VERSION_MISSING, NULL);
    } else {
	if (!xmlStrEqual(version, (const xmlChar *) XML_DEFAULT_VERSION)) {
	    /*
	     * TODO: Blueberry should be detected here
	     */
	    xmlWarningMsg(ctxt, XML_WAR_UNKNOWN_VERSION,
		          "Unsupported version '%s'\n",
			  version, NULL);
	}
	if (ctxt->version != NULL)
	    xmlFree((void *) ctxt->version);
	ctxt->version = version;
    }

    /*
     * We may have the encoding declaration
     */
    if (!IS_BLANK_CH(RAW)) {
        if ((RAW == '?') && (NXT(1) == '>')) {
	    SKIP(2);
	    return;
	}
	xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "Blank needed here\n");
    }
    xmlParseEncodingDecl(ctxt);
    if (ctxt->errNo == XML_ERR_UNSUPPORTED_ENCODING) {
	/*
	 * The XML REC instructs us to stop parsing right here
	 */
        return;
    }

    /*
     * We may have the standalone status.
     */
    if ((ctxt->input->encoding != NULL) && (!IS_BLANK_CH(RAW))) {
        if ((RAW == '?') && (NXT(1) == '>')) {
	    SKIP(2);
	    return;
	}
	xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "Blank needed here\n");
    }
    SKIP_BLANKS;
    ctxt->input->standalone = xmlParseSDDecl(ctxt);

    SKIP_BLANKS;
    if ((RAW == '?') && (NXT(1) == '>')) {
        SKIP(2);
    } else if (RAW == '>') {
        /* Deprecated old WD ... */
	xmlFatalErr(ctxt, XML_ERR_XMLDECL_NOT_FINISHED, NULL);
	NEXT;
    } else {
	xmlFatalErr(ctxt, XML_ERR_XMLDECL_NOT_FINISHED, NULL);
	MOVETO_ENDTAG(CUR_PTR);
	NEXT;
    }
}

/**
 * xmlParseMisc:
 * @ctxt:  an XML parser context
 * 
 * parse an XML Misc* optional field.
 *
 * [27] Misc ::= Comment | PI |  S
 */

void
xmlParseMisc(xmlParserCtxtPtr ctxt) {
    while (((RAW == '<') && (NXT(1) == '?')) ||
           (CMP4(CUR_PTR, '<', '!', '-', '-')) ||
           IS_BLANK_CH(CUR)) {
        if ((RAW == '<') && (NXT(1) == '?')) {
	    xmlParsePI(ctxt);
	} else if (IS_BLANK_CH(CUR)) {
	    NEXT;
	} else
	    xmlParseComment(ctxt);
    }
}

/**
 * xmlParseDocument:
 * @ctxt:  an XML parser context
 * 
 * parse an XML document (and build a tree if using the standard SAX
 * interface).
 *
 * [1] document ::= prolog element Misc*
 *
 * [22] prolog ::= XMLDecl? Misc* (doctypedecl Misc*)?
 *
 * Returns 0, -1 in case of error. the parser context is augmented
 *                as a result of the parsing.
 */

int
xmlParseDocument(xmlParserCtxtPtr ctxt) {
    xmlChar start[4];
    xmlCharEncoding enc;

    xmlInitParser();

    GROW;

    /*
     * SAX: detecting the level.
     */
    xmlDetectSAX2(ctxt);

    /*
     * SAX: beginning of the document processing.
     */
    if ((ctxt->sax) && (ctxt->sax->setDocumentLocator))
        ctxt->sax->setDocumentLocator(ctxt->userData, &xmlDefaultSAXLocator);

    if ((ctxt->encoding == (const xmlChar *)XML_CHAR_ENCODING_NONE) &&
        ((ctxt->input->end - ctxt->input->cur) >= 4)) {
	/* 
	 * Get the 4 first bytes and decode the charset
	 * if enc != XML_CHAR_ENCODING_NONE
	 * plug some encoding conversion routines.
	 */
	start[0] = RAW;
	start[1] = NXT(1);
	start[2] = NXT(2);
	start[3] = NXT(3);
	enc = xmlDetectCharEncoding(&start[0], 4);
	if (enc != XML_CHAR_ENCODING_NONE) {
	    xmlSwitchEncoding(ctxt, enc);
	}
    }


    if (CUR == 0) {
	xmlFatalErr(ctxt, XML_ERR_DOCUMENT_EMPTY, NULL);
    }

    /*
     * Check for the XMLDecl in the Prolog.
     */
    GROW;
    if ((CMP5(CUR_PTR, '<', '?', 'x', 'm', 'l')) && (IS_BLANK_CH(NXT(5)))) {

	/*
	 * Note that we will switch encoding on the fly.
	 */
	xmlParseXMLDecl(ctxt);
	if (ctxt->errNo == XML_ERR_UNSUPPORTED_ENCODING) {
	    /*
	     * The XML REC instructs us to stop parsing right here
	     */
	    return(-1);
	}
	ctxt->standalone = ctxt->input->standalone;
	SKIP_BLANKS;
    } else {
	ctxt->version = xmlCharStrdup(XML_DEFAULT_VERSION);
    }
    if ((ctxt->sax) && (ctxt->sax->startDocument) && (!ctxt->disableSAX))
        ctxt->sax->startDocument(ctxt->userData);

    /*
     * The Misc part of the Prolog
     */
    GROW;
    xmlParseMisc(ctxt);

    /*
     * Then possibly doc type declaration(s) and more Misc
     * (doctypedecl Misc*)?
     */
    GROW;
    if (CMP9(CUR_PTR, '<', '!', 'D', 'O', 'C', 'T', 'Y', 'P', 'E')) {

	ctxt->inSubset = 1;
	xmlParseDocTypeDecl(ctxt);
	if (RAW == '[') {
	    ctxt->instate = XML_PARSER_DTD;
	    xmlParseInternalSubset(ctxt);
	}

	/*
	 * Create and update the external subset.
	 */
	ctxt->inSubset = 2;
	if ((ctxt->sax != NULL) && (ctxt->sax->externalSubset != NULL) &&
	    (!ctxt->disableSAX))
	    ctxt->sax->externalSubset(ctxt->userData, ctxt->intSubName,
	                              ctxt->extSubSystem, ctxt->extSubURI);
	ctxt->inSubset = 0;


	ctxt->instate = XML_PARSER_PROLOG;
	xmlParseMisc(ctxt);
    }

    /*
     * Time to start parsing the tree itself
     */
    GROW;
    if (RAW != '<') {
	xmlFatalErrMsg(ctxt, XML_ERR_DOCUMENT_EMPTY,
		       "Start tag expected, '<' not found\n");
    } else {
	ctxt->instate = XML_PARSER_CONTENT;
	xmlParseElement(ctxt);
	ctxt->instate = XML_PARSER_EPILOG;


	/*
	 * The Misc part at the end
	 */
	xmlParseMisc(ctxt);

	if (RAW != 0) {
	    xmlFatalErr(ctxt, XML_ERR_DOCUMENT_END, NULL);
	}
	ctxt->instate = XML_PARSER_EOF;
    }

    /*
     * SAX: end of the document processing.
     */
    if ((ctxt->sax) && (ctxt->sax->endDocument != NULL))
        ctxt->sax->endDocument(ctxt->userData);

    /*
     * Remove locally kept entity definitions if the tree was not built
     */
    if ((ctxt->myDoc != NULL) &&
	(xmlStrEqual(ctxt->myDoc->version, SAX_COMPAT_MODE))) {
	xmlFreeDoc(ctxt->myDoc);
	ctxt->myDoc = NULL;
    }

    if (! ctxt->wellFormed) {
	ctxt->valid = 0;
	return(-1);
    }
    return(0);
}

/**
 * xmlParseExtParsedEnt:
 * @ctxt:  an XML parser context
 * 
 * parse a general parsed entity
 * An external general parsed entity is well-formed if it matches the
 * production labeled extParsedEnt.
 *
 * [78] extParsedEnt ::= TextDecl? content
 *
 * Returns 0, -1 in case of error. the parser context is augmented
 *                as a result of the parsing.
 */

int
xmlParseExtParsedEnt(xmlParserCtxtPtr ctxt) {
    xmlChar start[4];
    xmlCharEncoding enc;

    xmlDefaultSAXHandlerInit();

    xmlDetectSAX2(ctxt);

    GROW;

    /*
     * SAX: beginning of the document processing.
     */
    if ((ctxt->sax) && (ctxt->sax->setDocumentLocator))
        ctxt->sax->setDocumentLocator(ctxt->userData, &xmlDefaultSAXLocator);

    /* 
     * Get the 4 first bytes and decode the charset
     * if enc != XML_CHAR_ENCODING_NONE
     * plug some encoding conversion routines.
     */
    if ((ctxt->input->end - ctxt->input->cur) >= 4) {
	start[0] = RAW;
	start[1] = NXT(1);
	start[2] = NXT(2);
	start[3] = NXT(3);
	enc = xmlDetectCharEncoding(start, 4);
	if (enc != XML_CHAR_ENCODING_NONE) {
	    xmlSwitchEncoding(ctxt, enc);
	}
    }


    if (CUR == 0) {
	xmlFatalErr(ctxt, XML_ERR_DOCUMENT_EMPTY, NULL);
    }

    /*
     * Check for the XMLDecl in the Prolog.
     */
    GROW;
    if ((CMP5(CUR_PTR, '<', '?', 'x', 'm', 'l')) && (IS_BLANK_CH(NXT(5)))) {

	/*
	 * Note that we will switch encoding on the fly.
	 */
	xmlParseXMLDecl(ctxt);
	if (ctxt->errNo == XML_ERR_UNSUPPORTED_ENCODING) {
	    /*
	     * The XML REC instructs us to stop parsing right here
	     */
	    return(-1);
	}
	SKIP_BLANKS;
    } else {
	ctxt->version = xmlCharStrdup(XML_DEFAULT_VERSION);
    }
    if ((ctxt->sax) && (ctxt->sax->startDocument) && (!ctxt->disableSAX))
        ctxt->sax->startDocument(ctxt->userData);

    /*
     * Doing validity checking on chunk doesn't make sense
     */
    ctxt->instate = XML_PARSER_CONTENT;
    ctxt->validate = 0;
    ctxt->loadsubset = 0;
    ctxt->depth = 0;

    xmlParseContent(ctxt);
   
    if ((RAW == '<') && (NXT(1) == '/')) {
	xmlFatalErr(ctxt, XML_ERR_NOT_WELL_BALANCED, NULL);
    } else if (RAW != 0) {
	xmlFatalErr(ctxt, XML_ERR_EXTRA_CONTENT, NULL);
    }

    /*
     * SAX: end of the document processing.
     */
    if ((ctxt->sax) && (ctxt->sax->endDocument != NULL))
        ctxt->sax->endDocument(ctxt->userData);

    if (! ctxt->wellFormed) return(-1);
    return(0);
}

#ifdef LIBXML_PUSH_ENABLED
/************************************************************************
 *									*
 * 		Progressive parsing interfaces				*
 *									*
 ************************************************************************/

/**
 * xmlParseLookupSequence:
 * @ctxt:  an XML parser context
 * @first:  the first char to lookup
 * @next:  the next char to lookup or zero
 * @third:  the next char to lookup or zero
 *
 * Try to find if a sequence (first, next, third) or  just (first next) or
 * (first) is available in the input stream.
 * This function has a side effect of (possibly) incrementing ctxt->checkIndex
 * to avoid rescanning sequences of bytes, it DOES change the state of the
 * parser, do not use liberally.
 *
 * Returns the index to the current parsing point if the full sequence
 *      is available, -1 otherwise.
 */
static int
xmlParseLookupSequence(xmlParserCtxtPtr ctxt, xmlChar first,
                       xmlChar next, xmlChar third) {
    int base, len;
    xmlParserInputPtr in;
    const xmlChar *buf;

    in = ctxt->input;
    if (in == NULL) return(-1);
    base = in->cur - in->base;
    if (base < 0) return(-1);
    if (ctxt->checkIndex > base)
        base = ctxt->checkIndex;
    if (in->buf == NULL) {
	buf = in->base;
	len = in->length;
    } else {
	buf = in->buf->buffer->content;
	len = in->buf->buffer->use;
    }
    /* take into account the sequence length */
    if (third) len -= 2;
    else if (next) len --;
    for (;base < len;base++) {
        if (buf[base] == first) {
	    if (third != 0) {
		if ((buf[base + 1] != next) ||
		    (buf[base + 2] != third)) continue;
	    } else if (next != 0) {
		if (buf[base + 1] != next) continue;
	    }
	    ctxt->checkIndex = 0;
#ifdef DEBUG_PUSH
	    if (next == 0)
		xmlGenericError(xmlGenericErrorContext,
			"PP: lookup '%c' found at %d\n",
			first, base);
	    else if (third == 0)
		xmlGenericError(xmlGenericErrorContext,
			"PP: lookup '%c%c' found at %d\n",
			first, next, base);
	    else 
		xmlGenericError(xmlGenericErrorContext,
			"PP: lookup '%c%c%c' found at %d\n",
			first, next, third, base);
#endif
	    return(base - (in->cur - in->base));
	}
    }
    ctxt->checkIndex = base;
#ifdef DEBUG_PUSH
    if (next == 0)
	xmlGenericError(xmlGenericErrorContext,
		"PP: lookup '%c' failed\n", first);
    else if (third == 0)
	xmlGenericError(xmlGenericErrorContext,
		"PP: lookup '%c%c' failed\n", first, next);
    else	
	xmlGenericError(xmlGenericErrorContext,
		"PP: lookup '%c%c%c' failed\n", first, next, third);
#endif
    return(-1);
}

/**
 * xmlParseGetLasts:
 * @ctxt:  an XML parser context
 * @lastlt:  pointer to store the last '<' from the input
 * @lastgt:  pointer to store the last '>' from the input
 *
 * Lookup the last < and > in the current chunk
 */
static void
xmlParseGetLasts(xmlParserCtxtPtr ctxt, const xmlChar **lastlt,
                 const xmlChar **lastgt) {
    const xmlChar *tmp;

    if ((ctxt == NULL) || (lastlt == NULL) || (lastgt == NULL)) {
	xmlGenericError(xmlGenericErrorContext,
		    "Internal error: xmlParseGetLasts\n");
	return;
    }
    if ((ctxt->progressive == 1) && (ctxt->inputNr == 1)) {
        tmp = ctxt->input->end;
	tmp--;
	while ((tmp >= ctxt->input->base) && (*tmp != '<') &&
	       (*tmp != '>')) tmp--;
	if (tmp < ctxt->input->base) {
	    *lastlt = NULL;
	    *lastgt = NULL;
	} else if (*tmp == '<') {
	    *lastlt = tmp;
	    tmp--;
	    while ((tmp >= ctxt->input->base) && (*tmp != '>')) tmp--;
	    if (tmp < ctxt->input->base)
	        *lastgt = NULL;
	    else
	        *lastgt = tmp;
	} else {
	    *lastgt = tmp;
	    tmp--;
	    while ((tmp >= ctxt->input->base) && (*tmp != '<')) tmp--;
	    if (tmp < ctxt->input->base)
	        *lastlt = NULL;
	    else
	        *lastlt = tmp;
	}
        
    } else {
        *lastlt = NULL;
	*lastgt = NULL;
    }
}
/**
 * xmlParseTryOrFinish:
 * @ctxt:  an XML parser context
 * @terminate:  last chunk indicator
 *
 * Try to progress on parsing
 *
 * Returns zero if no parsing was possible
 */
static int
xmlParseTryOrFinish(xmlParserCtxtPtr ctxt, int terminate) {
    int ret = 0;
    int avail;
    xmlChar cur, next;
    const xmlChar *lastlt, *lastgt;

#ifdef DEBUG_PUSH
    switch (ctxt->instate) {
	case XML_PARSER_EOF:
	    xmlGenericError(xmlGenericErrorContext,
		    "PP: try EOF\n"); break;
	case XML_PARSER_START:
	    xmlGenericError(xmlGenericErrorContext,
		    "PP: try START\n"); break;
	case XML_PARSER_MISC:
	    xmlGenericError(xmlGenericErrorContext,
		    "PP: try MISC\n");break;
	case XML_PARSER_COMMENT:
	    xmlGenericError(xmlGenericErrorContext,
		    "PP: try COMMENT\n");break;
	case XML_PARSER_PROLOG:
	    xmlGenericError(xmlGenericErrorContext,
		    "PP: try PROLOG\n");break;
	case XML_PARSER_START_TAG:
	    xmlGenericError(xmlGenericErrorContext,
		    "PP: try START_TAG\n");break;
	case XML_PARSER_CONTENT:
	    xmlGenericError(xmlGenericErrorContext,
		    "PP: try CONTENT\n");break;
	case XML_PARSER_CDATA_SECTION:
	    xmlGenericError(xmlGenericErrorContext,
		    "PP: try CDATA_SECTION\n");break;
	case XML_PARSER_END_TAG:
	    xmlGenericError(xmlGenericErrorContext,
		    "PP: try END_TAG\n");break;
	case XML_PARSER_ENTITY_DECL:
	    xmlGenericError(xmlGenericErrorContext,
		    "PP: try ENTITY_DECL\n");break;
	case XML_PARSER_ENTITY_VALUE:
	    xmlGenericError(xmlGenericErrorContext,
		    "PP: try ENTITY_VALUE\n");break;
	case XML_PARSER_ATTRIBUTE_VALUE:
	    xmlGenericError(xmlGenericErrorContext,
		    "PP: try ATTRIBUTE_VALUE\n");break;
	case XML_PARSER_DTD:
	    xmlGenericError(xmlGenericErrorContext,
		    "PP: try DTD\n");break;
	case XML_PARSER_EPILOG:
	    xmlGenericError(xmlGenericErrorContext,
		    "PP: try EPILOG\n");break;
	case XML_PARSER_PI:
	    xmlGenericError(xmlGenericErrorContext,
		    "PP: try PI\n");break;
        case XML_PARSER_IGNORE:
            xmlGenericError(xmlGenericErrorContext,
		    "PP: try IGNORE\n");break;
    }
#endif

    if ((ctxt->input != NULL) &&
        (ctxt->input->cur - ctxt->input->base > 4096)) {
	xmlSHRINK(ctxt);
	ctxt->checkIndex = 0;
    }
    xmlParseGetLasts(ctxt, &lastlt, &lastgt);

    while (1) {
	if ((ctxt->errNo != XML_ERR_OK) && (ctxt->disableSAX == 1))
	    return(0);

        
	/*
	 * Pop-up of finished entities.
	 */
	while ((RAW == 0) && (ctxt->inputNr > 1))
	    xmlPopInput(ctxt);

	if (ctxt->input == NULL) break;
	if (ctxt->input->buf == NULL)
	    avail = ctxt->input->length -
	            (ctxt->input->cur - ctxt->input->base);
	else {
	    /*
	     * If we are operating on converted input, try to flush
	     * remainng chars to avoid them stalling in the non-converted
	     * buffer.
	     */
	    if ((ctxt->input->buf->raw != NULL) &&
		(ctxt->input->buf->raw->use > 0)) {
		int base = ctxt->input->base -
		           ctxt->input->buf->buffer->content;
		int current = ctxt->input->cur - ctxt->input->base;

		xmlParserInputBufferPush(ctxt->input->buf, 0, "");
		ctxt->input->base = ctxt->input->buf->buffer->content + base;
		ctxt->input->cur = ctxt->input->base + current;
		ctxt->input->end =
		    &ctxt->input->buf->buffer->content[
		                       ctxt->input->buf->buffer->use];
	    }
	    avail = ctxt->input->buf->buffer->use -
		    (ctxt->input->cur - ctxt->input->base);
	}
        if (avail < 1)
	    goto done;
        switch (ctxt->instate) {
            case XML_PARSER_EOF:
	        /*
		 * Document parsing is done !
		 */
	        goto done;
            case XML_PARSER_START:
		if (ctxt->charset == XML_CHAR_ENCODING_NONE) {
		    xmlChar start[4];
		    xmlCharEncoding enc;

		    /*
		     * Very first chars read from the document flow.
		     */
		    if (avail < 4)
			goto done;

		    /* 
		     * Get the 4 first bytes and decode the charset
		     * if enc != XML_CHAR_ENCODING_NONE
		     * plug some encoding conversion routines.
		     */
		    start[0] = RAW;
		    start[1] = NXT(1);
		    start[2] = NXT(2);
		    start[3] = NXT(3);
		    enc = xmlDetectCharEncoding(start, 4);
		    if (enc != XML_CHAR_ENCODING_NONE) {
			xmlSwitchEncoding(ctxt, enc);
		    }
		    break;
		}

		if (avail < 2)
		    goto done;
		cur = ctxt->input->cur[0];
		next = ctxt->input->cur[1];
		if (cur == 0) {
		    if ((ctxt->sax) && (ctxt->sax->setDocumentLocator))
			ctxt->sax->setDocumentLocator(ctxt->userData,
						      &xmlDefaultSAXLocator);
		    xmlFatalErr(ctxt, XML_ERR_DOCUMENT_EMPTY, NULL);
		    ctxt->instate = XML_PARSER_EOF;
#ifdef DEBUG_PUSH
		    xmlGenericError(xmlGenericErrorContext,
			    "PP: entering EOF\n");
#endif
		    if ((ctxt->sax) && (ctxt->sax->endDocument != NULL))
			ctxt->sax->endDocument(ctxt->userData);
		    goto done;
		}
	        if ((cur == '<') && (next == '?')) {
		    /* PI or XML decl */
		    if (avail < 5) return(ret);
		    if ((!terminate) &&
		        (xmlParseLookupSequence(ctxt, '?', '>', 0) < 0))
			return(ret);
		    if ((ctxt->sax) && (ctxt->sax->setDocumentLocator))
			ctxt->sax->setDocumentLocator(ctxt->userData,
						      &xmlDefaultSAXLocator);
		    if ((ctxt->input->cur[2] == 'x') &&
			(ctxt->input->cur[3] == 'm') &&
			(ctxt->input->cur[4] == 'l') &&
			(IS_BLANK_CH(ctxt->input->cur[5]))) {
			ret += 5;
#ifdef DEBUG_PUSH
			xmlGenericError(xmlGenericErrorContext,
				"PP: Parsing XML Decl\n");
#endif
			xmlParseXMLDecl(ctxt);
			if (ctxt->errNo == XML_ERR_UNSUPPORTED_ENCODING) {
			    /*
			     * The XML REC instructs us to stop parsing right
			     * here
			     */
			    ctxt->instate = XML_PARSER_EOF;
			    return(0);
			}
			ctxt->standalone = ctxt->input->standalone;
			if ((ctxt->encoding == NULL) &&
			    (ctxt->input->encoding != NULL))
			    ctxt->encoding = xmlStrdup(ctxt->input->encoding);
			if ((ctxt->sax) && (ctxt->sax->startDocument) &&
			    (!ctxt->disableSAX))
			    ctxt->sax->startDocument(ctxt->userData);
			ctxt->instate = XML_PARSER_MISC;
#ifdef DEBUG_PUSH
			xmlGenericError(xmlGenericErrorContext,
				"PP: entering MISC\n");
#endif
		    } else {
			ctxt->version = xmlCharStrdup(XML_DEFAULT_VERSION);
			if ((ctxt->sax) && (ctxt->sax->startDocument) &&
			    (!ctxt->disableSAX))
			    ctxt->sax->startDocument(ctxt->userData);
			ctxt->instate = XML_PARSER_MISC;
#ifdef DEBUG_PUSH
			xmlGenericError(xmlGenericErrorContext,
				"PP: entering MISC\n");
#endif
		    }
		} else {
		    if ((ctxt->sax) && (ctxt->sax->setDocumentLocator))
			ctxt->sax->setDocumentLocator(ctxt->userData,
						      &xmlDefaultSAXLocator);
		    ctxt->version = xmlCharStrdup(XML_DEFAULT_VERSION);
		    if ((ctxt->sax) && (ctxt->sax->startDocument) &&
		        (!ctxt->disableSAX))
			ctxt->sax->startDocument(ctxt->userData);
		    ctxt->instate = XML_PARSER_MISC;
#ifdef DEBUG_PUSH
		    xmlGenericError(xmlGenericErrorContext,
			    "PP: entering MISC\n");
#endif
		}
		break;
            case XML_PARSER_START_TAG: {
	        const xmlChar *name;
		const xmlChar *prefix;
		const xmlChar *URI;
		int nsNr = ctxt->nsNr;

		if ((avail < 2) && (ctxt->inputNr == 1))
		    goto done;
		cur = ctxt->input->cur[0];
	        if (cur != '<') {
		    xmlFatalErr(ctxt, XML_ERR_DOCUMENT_EMPTY, NULL);
		    ctxt->instate = XML_PARSER_EOF;
		    if ((ctxt->sax) && (ctxt->sax->endDocument != NULL))
			ctxt->sax->endDocument(ctxt->userData);
		    goto done;
		}
		if (!terminate) {
		    if (ctxt->progressive) {
		        if ((lastgt == NULL) || (ctxt->input->cur > lastgt))
			    goto done;
		    } else if (xmlParseLookupSequence(ctxt, '>', 0, 0) < 0) {
			goto done;
		    }
		}
		if (ctxt->spaceNr == 0)
		    spacePush(ctxt, -1);
		else
		    spacePush(ctxt, *ctxt->space);
#ifdef LIBXML_SAX1_ENABLED
		if (ctxt->sax2)
#endif /* LIBXML_SAX1_ENABLED */
		    name = xmlParseStartTag2(ctxt, &prefix, &URI);
#ifdef LIBXML_SAX1_ENABLED
		else
		    name = xmlParseStartTag(ctxt);
#endif /* LIBXML_SAX1_ENABLED */
		if (name == NULL) {
		    spacePop(ctxt);
		    ctxt->instate = XML_PARSER_EOF;
		    if ((ctxt->sax) && (ctxt->sax->endDocument != NULL))
			ctxt->sax->endDocument(ctxt->userData);
		    goto done;
		}
#ifdef LIBXML_VALID_ENABLED
		/*
		 * [ VC: Root Element Type ]
		 * The Name in the document type declaration must match
		 * the element type of the root element. 
		 */
		if (ctxt->validate && ctxt->wellFormed && ctxt->myDoc &&
		    ctxt->node && (ctxt->node == ctxt->myDoc->children))
		    ctxt->valid &= xmlValidateRoot(&ctxt->vctxt, ctxt->myDoc);
#endif /* LIBXML_VALID_ENABLED */

		/*
		 * Check for an Empty Element.
		 */
		if ((RAW == '/') && (NXT(1) == '>')) {
		    SKIP(2);

		    if (ctxt->sax2) {
			if ((ctxt->sax != NULL) &&
			    (ctxt->sax->endElementNs != NULL) &&
			    (!ctxt->disableSAX))
			    ctxt->sax->endElementNs(ctxt->userData, name,
			                            prefix, URI);
#ifdef LIBXML_SAX1_ENABLED
		    } else {
			if ((ctxt->sax != NULL) &&
			    (ctxt->sax->endElement != NULL) &&
			    (!ctxt->disableSAX))
			    ctxt->sax->endElement(ctxt->userData, name);
#endif /* LIBXML_SAX1_ENABLED */
		    }
		    spacePop(ctxt);
		    if (ctxt->nameNr == 0) {
			ctxt->instate = XML_PARSER_EPILOG;
		    } else {
			ctxt->instate = XML_PARSER_CONTENT;
		    }
		    break;
		}
		if (RAW == '>') {
		    NEXT;
		} else {
		    xmlFatalErrMsgStr(ctxt, XML_ERR_GT_REQUIRED,
					 "Couldn't find end of Start Tag %s\n",
					 name);
		    nodePop(ctxt);
		    spacePop(ctxt);
		}
		if (ctxt->sax2)
		    nameNsPush(ctxt, name, prefix, URI, ctxt->nsNr - nsNr);
#ifdef LIBXML_SAX1_ENABLED
		else
		    namePush(ctxt, name);
#endif /* LIBXML_SAX1_ENABLED */

		ctxt->instate = XML_PARSER_CONTENT;
                break;
	    }
            case XML_PARSER_CONTENT: {
		const xmlChar *test;
		unsigned int cons;
		if ((avail < 2) && (ctxt->inputNr == 1))
		    goto done;
		cur = ctxt->input->cur[0];
		next = ctxt->input->cur[1];

		test = CUR_PTR;
	        cons = ctxt->input->consumed;
		if ((cur == '<') && (next == '/')) {
		    ctxt->instate = XML_PARSER_END_TAG;
		    break;
	        } else if ((cur == '<') && (next == '?')) {
		    if ((!terminate) &&
		        (xmlParseLookupSequence(ctxt, '?', '>', 0) < 0))
			goto done;
		    xmlParsePI(ctxt);
		} else if ((cur == '<') && (next != '!')) {
		    ctxt->instate = XML_PARSER_START_TAG;
		    break;
		} else if ((cur == '<') && (next == '!') &&
		           (ctxt->input->cur[2] == '-') &&
			   (ctxt->input->cur[3] == '-')) {
		    if ((!terminate) &&
		        (xmlParseLookupSequence(ctxt, '-', '-', '>') < 0))
			goto done;
		    xmlParseComment(ctxt);
		    ctxt->instate = XML_PARSER_CONTENT;
		} else if ((cur == '<') && (ctxt->input->cur[1] == '!') &&
		    (ctxt->input->cur[2] == '[') &&
		    (ctxt->input->cur[3] == 'C') &&
		    (ctxt->input->cur[4] == 'D') &&
		    (ctxt->input->cur[5] == 'A') &&
		    (ctxt->input->cur[6] == 'T') &&
		    (ctxt->input->cur[7] == 'A') &&
		    (ctxt->input->cur[8] == '[')) {
		    SKIP(9);
		    ctxt->instate = XML_PARSER_CDATA_SECTION;
		    break;
		} else if ((cur == '<') && (next == '!') &&
		           (avail < 9)) {
		    goto done;
		} else if (cur == '&') {
		    if ((!terminate) &&
		        (xmlParseLookupSequence(ctxt, ';', 0, 0) < 0))
			goto done;
		    xmlParseReference(ctxt);
		} else {
		    /* TODO Avoid the extra copy, handle directly !!! */
		    /*
		     * Goal of the following test is:
		     *  - minimize calls to the SAX 'character' callback
		     *    when they are mergeable
		     *  - handle an problem for isBlank when we only parse
		     *    a sequence of blank chars and the next one is
		     *    not available to check against '<' presence.
		     *  - tries to homogenize the differences in SAX
		     *    callbacks between the push and pull versions
		     *    of the parser.
		     */
		    if ((ctxt->inputNr == 1) &&
		        (avail < XML_PARSER_BIG_BUFFER_SIZE)) {
			if (!terminate) {
			    if (ctxt->progressive) {
				if ((lastlt == NULL) ||
				    (ctxt->input->cur > lastlt))
				    goto done;
			    } else if (xmlParseLookupSequence(ctxt,
			                                      '<', 0, 0) < 0) {
				goto done;
			    }
			}
                    }
		    ctxt->checkIndex = 0;
		    xmlParseCharData(ctxt, 0);
		}
		/*
		 * Pop-up of finished entities.
		 */
		while ((RAW == 0) && (ctxt->inputNr > 1))
		    xmlPopInput(ctxt);
		if ((cons == ctxt->input->consumed) && (test == CUR_PTR)) {
		    xmlFatalErr(ctxt, XML_ERR_INTERNAL_ERROR,
		                "detected an error in element content\n");
		    ctxt->instate = XML_PARSER_EOF;
		    break;
		}
		break;
	    }
            case XML_PARSER_END_TAG:
		if (avail < 2)
		    goto done;
		if (!terminate) {
		    if (ctxt->progressive) {
		        if ((lastgt == NULL) || (ctxt->input->cur > lastgt))
			    goto done;
		    } else if (xmlParseLookupSequence(ctxt, '>', 0, 0) < 0) {
			goto done;
		    }
		}
		if (ctxt->sax2) {
		    xmlParseEndTag2(ctxt,
		           (void *) ctxt->pushTab[ctxt->nameNr * 3 - 3],
		           (void *) ctxt->pushTab[ctxt->nameNr * 3 - 2], 0,
		       (int) (long) ctxt->pushTab[ctxt->nameNr * 3 - 1]);
		    nameNsPop(ctxt);
		}
#ifdef LIBXML_SAX1_ENABLED
		  else
		    xmlParseEndTag1(ctxt, 0);
#endif /* LIBXML_SAX1_ENABLED */
		if (ctxt->nameNr == 0) {
		    ctxt->instate = XML_PARSER_EPILOG;
		} else {
		    ctxt->instate = XML_PARSER_CONTENT;
		}
		break;
            case XML_PARSER_CDATA_SECTION: {
	        /*
		 * The Push mode need to have the SAX callback for 
		 * cdataBlock merge back contiguous callbacks.
		 */
		int base;

		base = xmlParseLookupSequence(ctxt, ']', ']', '>');
		if (base < 0) {
		    if (avail >= XML_PARSER_BIG_BUFFER_SIZE + 2) {
			if ((ctxt->sax != NULL) && (!ctxt->disableSAX)) {
			    if (ctxt->sax->cdataBlock != NULL)
				ctxt->sax->cdataBlock(ctxt->userData,
				                      ctxt->input->cur,
					  XML_PARSER_BIG_BUFFER_SIZE);
			    else if (ctxt->sax->characters != NULL)
				ctxt->sax->characters(ctxt->userData,
				                      ctxt->input->cur,
					  XML_PARSER_BIG_BUFFER_SIZE);
			}
			SKIP(XML_PARSER_BIG_BUFFER_SIZE);
			ctxt->checkIndex = 0;
		    }
		    goto done;
		} else {
		    if ((ctxt->sax != NULL) && (base > 0) &&
			(!ctxt->disableSAX)) {
			if (ctxt->sax->cdataBlock != NULL)
			    ctxt->sax->cdataBlock(ctxt->userData,
						  ctxt->input->cur, base);
			else if (ctxt->sax->characters != NULL)
			    ctxt->sax->characters(ctxt->userData,
						  ctxt->input->cur, base);
		    }
		    SKIP(base + 3);
		    ctxt->checkIndex = 0;
		    ctxt->instate = XML_PARSER_CONTENT;
#ifdef DEBUG_PUSH
		    xmlGenericError(xmlGenericErrorContext,
			    "PP: entering CONTENT\n");
#endif
		}
		break;
	    }
            case XML_PARSER_MISC:
		SKIP_BLANKS;
		if (ctxt->input->buf == NULL)
		    avail = ctxt->input->length -
		            (ctxt->input->cur - ctxt->input->base);
		else
		    avail = ctxt->input->buf->buffer->use -
		            (ctxt->input->cur - ctxt->input->base);
		if (avail < 2)
		    goto done;
		cur = ctxt->input->cur[0];
		next = ctxt->input->cur[1];
	        if ((cur == '<') && (next == '?')) {
		    if ((!terminate) &&
		        (xmlParseLookupSequence(ctxt, '?', '>', 0) < 0))
			goto done;
#ifdef DEBUG_PUSH
		    xmlGenericError(xmlGenericErrorContext,
			    "PP: Parsing PI\n");
#endif
		    xmlParsePI(ctxt);
		} else if ((cur == '<') && (next == '!') &&
		    (ctxt->input->cur[2] == '-') &&
		    (ctxt->input->cur[3] == '-')) {
		    if ((!terminate) &&
		        (xmlParseLookupSequence(ctxt, '-', '-', '>') < 0))
			goto done;
#ifdef DEBUG_PUSH
		    xmlGenericError(xmlGenericErrorContext,
			    "PP: Parsing Comment\n");
#endif
		    xmlParseComment(ctxt);
		    ctxt->instate = XML_PARSER_MISC;
		} else if ((cur == '<') && (next == '!') &&
		    (ctxt->input->cur[2] == 'D') &&
		    (ctxt->input->cur[3] == 'O') &&
		    (ctxt->input->cur[4] == 'C') &&
		    (ctxt->input->cur[5] == 'T') &&
		    (ctxt->input->cur[6] == 'Y') &&
		    (ctxt->input->cur[7] == 'P') &&
		    (ctxt->input->cur[8] == 'E')) {
		    if ((!terminate) &&
		        (xmlParseLookupSequence(ctxt, '>', 0, 0) < 0))
			goto done;
#ifdef DEBUG_PUSH
		    xmlGenericError(xmlGenericErrorContext,
			    "PP: Parsing internal subset\n");
#endif
		    ctxt->inSubset = 1;
		    xmlParseDocTypeDecl(ctxt);
		    if (RAW == '[') {
			ctxt->instate = XML_PARSER_DTD;
#ifdef DEBUG_PUSH
			xmlGenericError(xmlGenericErrorContext,
				"PP: entering DTD\n");
#endif
		    } else {
			/*
			 * Create and update the external subset.
			 */
			ctxt->inSubset = 2;
			if ((ctxt->sax != NULL) && (!ctxt->disableSAX) &&
			    (ctxt->sax->externalSubset != NULL))
			    ctxt->sax->externalSubset(ctxt->userData,
				    ctxt->intSubName, ctxt->extSubSystem,
				    ctxt->extSubURI);
			ctxt->inSubset = 0;
			ctxt->instate = XML_PARSER_PROLOG;
#ifdef DEBUG_PUSH
			xmlGenericError(xmlGenericErrorContext,
				"PP: entering PROLOG\n");
#endif
		    }
		} else if ((cur == '<') && (next == '!') &&
		           (avail < 9)) {
		    goto done;
		} else {
		    ctxt->instate = XML_PARSER_START_TAG;
		    ctxt->progressive = 1;
		    xmlParseGetLasts(ctxt, &lastlt, &lastgt);
#ifdef DEBUG_PUSH
		    xmlGenericError(xmlGenericErrorContext,
			    "PP: entering START_TAG\n");
#endif
		}
		break;
            case XML_PARSER_PROLOG:
		SKIP_BLANKS;
		if (ctxt->input->buf == NULL)
		    avail = ctxt->input->length - (ctxt->input->cur - ctxt->input->base);
		else
		    avail = ctxt->input->buf->buffer->use - (ctxt->input->cur - ctxt->input->base);
		if (avail < 2) 
		    goto done;
		cur = ctxt->input->cur[0];
		next = ctxt->input->cur[1];
	        if ((cur == '<') && (next == '?')) {
		    if ((!terminate) &&
		        (xmlParseLookupSequence(ctxt, '?', '>', 0) < 0))
			goto done;
#ifdef DEBUG_PUSH
		    xmlGenericError(xmlGenericErrorContext,
			    "PP: Parsing PI\n");
#endif
		    xmlParsePI(ctxt);
		} else if ((cur == '<') && (next == '!') &&
		    (ctxt->input->cur[2] == '-') && (ctxt->input->cur[3] == '-')) {
		    if ((!terminate) &&
		        (xmlParseLookupSequence(ctxt, '-', '-', '>') < 0))
			goto done;
#ifdef DEBUG_PUSH
		    xmlGenericError(xmlGenericErrorContext,
			    "PP: Parsing Comment\n");
#endif
		    xmlParseComment(ctxt);
		    ctxt->instate = XML_PARSER_PROLOG;
		} else if ((cur == '<') && (next == '!') &&
		           (avail < 4)) {
		    goto done;
		} else {
		    ctxt->instate = XML_PARSER_START_TAG;
		    ctxt->progressive = 1;
		    xmlParseGetLasts(ctxt, &lastlt, &lastgt);
#ifdef DEBUG_PUSH
		    xmlGenericError(xmlGenericErrorContext,
			    "PP: entering START_TAG\n");
#endif
		}
		break;
            case XML_PARSER_EPILOG:
		SKIP_BLANKS;
		if (ctxt->input->buf == NULL)
		    avail = ctxt->input->length - (ctxt->input->cur - ctxt->input->base);
		else
		    avail = ctxt->input->buf->buffer->use - (ctxt->input->cur - ctxt->input->base);
		if (avail < 2)
		    goto done;
		cur = ctxt->input->cur[0];
		next = ctxt->input->cur[1];
	        if ((cur == '<') && (next == '?')) {
		    if ((!terminate) &&
		        (xmlParseLookupSequence(ctxt, '?', '>', 0) < 0))
			goto done;
#ifdef DEBUG_PUSH
		    xmlGenericError(xmlGenericErrorContext,
			    "PP: Parsing PI\n");
#endif
		    xmlParsePI(ctxt);
		    ctxt->instate = XML_PARSER_EPILOG;
		} else if ((cur == '<') && (next == '!') &&
		    (ctxt->input->cur[2] == '-') && (ctxt->input->cur[3] == '-')) {
		    if ((!terminate) &&
		        (xmlParseLookupSequence(ctxt, '-', '-', '>') < 0))
			goto done;
#ifdef DEBUG_PUSH
		    xmlGenericError(xmlGenericErrorContext,
			    "PP: Parsing Comment\n");
#endif
		    xmlParseComment(ctxt);
		    ctxt->instate = XML_PARSER_EPILOG;
		} else if ((cur == '<') && (next == '!') &&
		           (avail < 4)) {
		    goto done;
		} else {
		    xmlFatalErr(ctxt, XML_ERR_DOCUMENT_END, NULL);
		    ctxt->instate = XML_PARSER_EOF;
#ifdef DEBUG_PUSH
		    xmlGenericError(xmlGenericErrorContext,
			    "PP: entering EOF\n");
#endif
		    if ((ctxt->sax) && (ctxt->sax->endDocument != NULL))
			ctxt->sax->endDocument(ctxt->userData);
		    goto done;
		}
		break;
            case XML_PARSER_DTD: {
	        /*
		 * Sorry but progressive parsing of the internal subset
		 * is not expected to be supported. We first check that
		 * the full content of the internal subset is available and
		 * the parsing is launched only at that point.
		 * Internal subset ends up with "']' S? '>'" in an unescaped
		 * section and not in a ']]>' sequence which are conditional
		 * sections (whoever argued to keep that crap in XML deserve
		 * a place in hell !).
		 */
		int base, i;
		xmlChar *buf;
	        xmlChar quote = 0;

		base = ctxt->input->cur - ctxt->input->base;
		if (base < 0) return(0);
		if (ctxt->checkIndex > base)
		    base = ctxt->checkIndex;
		buf = ctxt->input->buf->buffer->content;
		for (;(unsigned int) base < ctxt->input->buf->buffer->use;
		     base++) {
		    if (quote != 0) {
		        if (buf[base] == quote)
			    quote = 0;
			continue;    
		    }
		    if (buf[base] == '"') {
		        quote = '"';
			continue;
		    }
		    if (buf[base] == '\'') {
		        quote = '\'';
			continue;
		    }
		    if (buf[base] == ']') {
		        if ((unsigned int) base +1 >=
		            ctxt->input->buf->buffer->use)
			    break;
			if (buf[base + 1] == ']') {
			    /* conditional crap, skip both ']' ! */
			    base++;
			    continue;
			}
		        for (i = 0;
		     (unsigned int) base + i < ctxt->input->buf->buffer->use;
		             i++) {
			    if (buf[base + i] == '>')
			        goto found_end_int_subset;
			}
		        break;
		    }
		}
		/*
		 * We didn't found the end of the Internal subset
		 */
		if (quote == 0) 
		    ctxt->checkIndex = base;
#ifdef DEBUG_PUSH
		if (next == 0)
		    xmlGenericError(xmlGenericErrorContext,
			    "PP: lookup of int subset end filed\n");
#endif
	        goto done;

found_end_int_subset:
		xmlParseInternalSubset(ctxt);
		ctxt->inSubset = 2;
		if ((ctxt->sax != NULL) && (!ctxt->disableSAX) &&
		    (ctxt->sax->externalSubset != NULL))
		    ctxt->sax->externalSubset(ctxt->userData, ctxt->intSubName,
			    ctxt->extSubSystem, ctxt->extSubURI);
		ctxt->inSubset = 0;
		ctxt->instate = XML_PARSER_PROLOG;
		ctxt->checkIndex = 0;
#ifdef DEBUG_PUSH
		xmlGenericError(xmlGenericErrorContext,
			"PP: entering PROLOG\n");
#endif
                break;
	    }
            case XML_PARSER_COMMENT:
		xmlGenericError(xmlGenericErrorContext,
			"PP: internal error, state == COMMENT\n");
		ctxt->instate = XML_PARSER_CONTENT;
#ifdef DEBUG_PUSH
		xmlGenericError(xmlGenericErrorContext,
			"PP: entering CONTENT\n");
#endif
		break;
            case XML_PARSER_IGNORE:
		xmlGenericError(xmlGenericErrorContext,
			"PP: internal error, state == IGNORE");
	        ctxt->instate = XML_PARSER_DTD;
#ifdef DEBUG_PUSH
		xmlGenericError(xmlGenericErrorContext,
			"PP: entering DTD\n");
#endif
	        break;
            case XML_PARSER_PI:
		xmlGenericError(xmlGenericErrorContext,
			"PP: internal error, state == PI\n");
		ctxt->instate = XML_PARSER_CONTENT;
#ifdef DEBUG_PUSH
		xmlGenericError(xmlGenericErrorContext,
			"PP: entering CONTENT\n");
#endif
		break;
            case XML_PARSER_ENTITY_DECL:
		xmlGenericError(xmlGenericErrorContext,
			"PP: internal error, state == ENTITY_DECL\n");
		ctxt->instate = XML_PARSER_DTD;
#ifdef DEBUG_PUSH
		xmlGenericError(xmlGenericErrorContext,
			"PP: entering DTD\n");
#endif
		break;
            case XML_PARSER_ENTITY_VALUE:
		xmlGenericError(xmlGenericErrorContext,
			"PP: internal error, state == ENTITY_VALUE\n");
		ctxt->instate = XML_PARSER_CONTENT;
#ifdef DEBUG_PUSH
		xmlGenericError(xmlGenericErrorContext,
			"PP: entering DTD\n");
#endif
		break;
            case XML_PARSER_ATTRIBUTE_VALUE:
		xmlGenericError(xmlGenericErrorContext,
			"PP: internal error, state == ATTRIBUTE_VALUE\n");
		ctxt->instate = XML_PARSER_START_TAG;
#ifdef DEBUG_PUSH
		xmlGenericError(xmlGenericErrorContext,
			"PP: entering START_TAG\n");
#endif
		break;
            case XML_PARSER_SYSTEM_LITERAL:
		xmlGenericError(xmlGenericErrorContext,
			"PP: internal error, state == SYSTEM_LITERAL\n");
		ctxt->instate = XML_PARSER_START_TAG;
#ifdef DEBUG_PUSH
		xmlGenericError(xmlGenericErrorContext,
			"PP: entering START_TAG\n");
#endif
		break;
            case XML_PARSER_PUBLIC_LITERAL:
		xmlGenericError(xmlGenericErrorContext,
			"PP: internal error, state == PUBLIC_LITERAL\n");
		ctxt->instate = XML_PARSER_START_TAG;
#ifdef DEBUG_PUSH
		xmlGenericError(xmlGenericErrorContext,
			"PP: entering START_TAG\n");
#endif
		break;
	}
    }
done:    
#ifdef DEBUG_PUSH
    xmlGenericError(xmlGenericErrorContext, "PP: done %d\n", ret);
#endif
    return(ret);
}

/**
 * xmlParseChunk:
 * @ctxt:  an XML parser context
 * @chunk:  an char array
 * @size:  the size in byte of the chunk
 * @terminate:  last chunk indicator
 *
 * Parse a Chunk of memory
 *
 * Returns zero if no error, the xmlParserErrors otherwise.
 */
int
xmlParseChunk(xmlParserCtxtPtr ctxt, const char *chunk, int size,
              int terminate) {
    if ((ctxt->errNo != XML_ERR_OK) && (ctxt->disableSAX == 1))
        return(ctxt->errNo);
    if (ctxt->instate == XML_PARSER_START)
        xmlDetectSAX2(ctxt);
    if ((size > 0) && (chunk != NULL) && (ctxt->input != NULL) &&
        (ctxt->input->buf != NULL) && (ctxt->instate != XML_PARSER_EOF))  {
	int base = ctxt->input->base - ctxt->input->buf->buffer->content;
	int cur = ctxt->input->cur - ctxt->input->base;
	
	xmlParserInputBufferPush(ctxt->input->buf, size, chunk);	      
	ctxt->input->base = ctxt->input->buf->buffer->content + base;
	ctxt->input->cur = ctxt->input->base + cur;
	ctxt->input->end =
	    &ctxt->input->buf->buffer->content[ctxt->input->buf->buffer->use];
#ifdef DEBUG_PUSH
	xmlGenericError(xmlGenericErrorContext, "PP: pushed %d\n", size);
#endif

    } else if (ctxt->instate != XML_PARSER_EOF) {
	if ((ctxt->input != NULL) && ctxt->input->buf != NULL) {
	    xmlParserInputBufferPtr in = ctxt->input->buf;
	    if ((in->encoder != NULL) && (in->buffer != NULL) &&
		    (in->raw != NULL)) {
		int nbchars;
		    
		nbchars = xmlCharEncInFunc(in->encoder, in->buffer, in->raw);
		if (nbchars < 0) {
		    /* TODO 2.6.0 */
		    xmlGenericError(xmlGenericErrorContext,
				    "xmlParseChunk: encoder error\n");
		    return(XML_ERR_INVALID_ENCODING);
		}
	    }
	}
    }
    xmlParseTryOrFinish(ctxt, terminate);
    if ((ctxt->errNo != XML_ERR_OK) && (ctxt->disableSAX == 1))
        return(ctxt->errNo);
    if (terminate) {
	/*
	 * Check for termination
	 */
	    int avail = 0;
	    if (ctxt->input->buf == NULL)
                avail = ctxt->input->length -
		        (ctxt->input->cur - ctxt->input->base);
            else
                avail = ctxt->input->buf->buffer->use -
		        (ctxt->input->cur - ctxt->input->base);
			    
	if ((ctxt->instate != XML_PARSER_EOF) &&
	    (ctxt->instate != XML_PARSER_EPILOG)) {
	    xmlFatalErr(ctxt, XML_ERR_DOCUMENT_END, NULL);
	} 
	if ((ctxt->instate == XML_PARSER_EPILOG) && (avail > 0)) {
	    xmlFatalErr(ctxt, XML_ERR_DOCUMENT_END, NULL);
	}
	if (ctxt->instate != XML_PARSER_EOF) {
	    if ((ctxt->sax) && (ctxt->sax->endDocument != NULL))
		ctxt->sax->endDocument(ctxt->userData);
	}
	ctxt->instate = XML_PARSER_EOF;
    }
    return((xmlParserErrors) ctxt->errNo);	      
}

/************************************************************************
 *									*
 * 		I/O front end functions to the parser			*
 *									*
 ************************************************************************/

/**
 * xmlStopParser:
 * @ctxt:  an XML parser context
 *
 * Blocks further parser processing
 */
void           
xmlStopParser(xmlParserCtxtPtr ctxt) {
    ctxt->instate = XML_PARSER_EOF;
    if (ctxt->input != NULL)
	ctxt->input->cur = BAD_CAST"";
}

/**
 * xmlCreatePushParserCtxt:
 * @sax:  a SAX handler
 * @user_data:  The user data returned on SAX callbacks
 * @chunk:  a pointer to an array of chars
 * @size:  number of chars in the array
 * @filename:  an optional file name or URI
 *
 * Create a parser context for using the XML parser in push mode.
 * If @buffer and @size are non-NULL, the data is used to detect
 * the encoding.  The remaining characters will be parsed so they
 * don't need to be fed in again through xmlParseChunk.
 * To allow content encoding detection, @size should be >= 4
 * The value of @filename is used for fetching external entities
 * and error/warning reports.
 *
 * Returns the new parser context or NULL
 */

xmlParserCtxtPtr
xmlCreatePushParserCtxt(xmlSAXHandlerPtr sax, void *user_data, 
                        const char *chunk, int size, const char *filename) {
    xmlParserCtxtPtr ctxt;
    xmlParserInputPtr inputStream;
    xmlParserInputBufferPtr buf;
    xmlCharEncoding enc = XML_CHAR_ENCODING_NONE;

    /*
     * plug some encoding conversion routines
     */
    if ((chunk != NULL) && (size >= 4))
	enc = xmlDetectCharEncoding((const xmlChar *) chunk, size);

    buf = xmlAllocParserInputBuffer(enc);
    if (buf == NULL) return(NULL);

    ctxt = xmlNewParserCtxt();
    if (ctxt == NULL) {
        xmlErrMemory(NULL, "creating parser: out of memory\n");
	xmlFreeParserInputBuffer(buf);
	return(NULL);
    }
    ctxt->pushTab = (void **) xmlMalloc(ctxt->nameMax * 3 * sizeof(xmlChar *));
    if (ctxt->pushTab == NULL) {
        xmlErrMemory(ctxt, NULL);
	xmlFreeParserInputBuffer(buf);
	xmlFreeParserCtxt(ctxt);
	return(NULL);
    }
    if (sax != NULL) {
#ifdef LIBXML_SAX1_ENABLED
	if (ctxt->sax != (xmlSAXHandlerPtr) &xmlDefaultSAXHandler)
#endif /* LIBXML_SAX1_ENABLED */
	    xmlFree(ctxt->sax);
	ctxt->sax = (xmlSAXHandlerPtr) xmlMalloc(sizeof(xmlSAXHandler));
	if (ctxt->sax == NULL) {
	    xmlErrMemory(ctxt, NULL);
	    xmlFreeParserInputBuffer(buf);
	    xmlFreeParserCtxt(ctxt);
	    return(NULL);
	}
	memcpy(ctxt->sax, sax, sizeof(xmlSAXHandler));
	if (user_data != NULL)
	    ctxt->userData = user_data;
    }	
    if (filename == NULL) {
	ctxt->directory = NULL;
    } else {
        ctxt->directory = xmlParserGetDirectory(filename);
    }

    inputStream = xmlNewInputStream(ctxt);
    if (inputStream == NULL) {
	xmlFreeParserCtxt(ctxt);
	xmlFreeParserInputBuffer(buf);
	return(NULL);
    }

    if (filename == NULL)
	inputStream->filename = NULL;
    else
	inputStream->filename = (char *)
	    xmlCanonicPath((const xmlChar *) filename);
    inputStream->buf = buf;
    inputStream->base = inputStream->buf->buffer->content;
    inputStream->cur = inputStream->buf->buffer->content;
    inputStream->end = 
	&inputStream->buf->buffer->content[inputStream->buf->buffer->use];

    inputPush(ctxt, inputStream);

    if ((size > 0) && (chunk != NULL) && (ctxt->input != NULL) &&
        (ctxt->input->buf != NULL))  {	      
	int base = ctxt->input->base - ctxt->input->buf->buffer->content;
	int cur = ctxt->input->cur - ctxt->input->base;

	xmlParserInputBufferPush(ctxt->input->buf, size, chunk);	      

	ctxt->input->base = ctxt->input->buf->buffer->content + base;
	ctxt->input->cur = ctxt->input->base + cur;
	ctxt->input->end =
	    &ctxt->input->buf->buffer->content[ctxt->input->buf->buffer->use];
#ifdef DEBUG_PUSH
	xmlGenericError(xmlGenericErrorContext, "PP: pushed %d\n", size);
#endif
    }

    if (enc != XML_CHAR_ENCODING_NONE) {
        xmlSwitchEncoding(ctxt, enc);
    }

    return(ctxt);
}
#endif /* LIBXML_PUSH_ENABLED */

/**
 * xmlCreateIOParserCtxt:
 * @sax:  a SAX handler
 * @user_data:  The user data returned on SAX callbacks
 * @ioread:  an I/O read function
 * @ioclose:  an I/O close function
 * @ioctx:  an I/O handler
 * @enc:  the charset encoding if known
 *
 * Create a parser context for using the XML parser with an existing
 * I/O stream
 *
 * Returns the new parser context or NULL
 */
xmlParserCtxtPtr
xmlCreateIOParserCtxt(xmlSAXHandlerPtr sax, void *user_data,
	xmlInputReadCallback   ioread, xmlInputCloseCallback  ioclose,
	void *ioctx, xmlCharEncoding enc) {
    xmlParserCtxtPtr ctxt;
    xmlParserInputPtr inputStream;
    xmlParserInputBufferPtr buf;

    buf = xmlParserInputBufferCreateIO(ioread, ioclose, ioctx, enc);
    if (buf == NULL) return(NULL);

    ctxt = xmlNewParserCtxt();
    if (ctxt == NULL) {
	xmlFree(buf);
	return(NULL);
    }
    if (sax != NULL) {
#ifdef LIBXML_SAX1_ENABLED
	if (ctxt->sax != (xmlSAXHandlerPtr) &xmlDefaultSAXHandler)
#endif /* LIBXML_SAX1_ENABLED */
	    xmlFree(ctxt->sax);
	ctxt->sax = (xmlSAXHandlerPtr) xmlMalloc(sizeof(xmlSAXHandler));
	if (ctxt->sax == NULL) {
	    xmlErrMemory(ctxt, NULL);
	    xmlFree(ctxt);
	    return(NULL);
	}
	memcpy(ctxt->sax, sax, sizeof(xmlSAXHandler));
	if (user_data != NULL)
	    ctxt->userData = user_data;
    }	

    inputStream = xmlNewIOInputStream(ctxt, buf, enc);
    if (inputStream == NULL) {
	xmlFreeParserCtxt(ctxt);
	return(NULL);
    }
    inputPush(ctxt, inputStream);

    return(ctxt);
}

#ifdef LIBXML_VALID_ENABLED
/************************************************************************
 *									*
 * 		Front ends when parsing a DTD				*
 *									*
 ************************************************************************/

/**
 * xmlIOParseDTD:
 * @sax:  the SAX handler block or NULL
 * @input:  an Input Buffer
 * @enc:  the charset encoding if known
 *
 * Load and parse a DTD
 * 
 * Returns the resulting xmlDtdPtr or NULL in case of error.
 * @input will be freed at parsing end.
 */

xmlDtdPtr
xmlIOParseDTD(xmlSAXHandlerPtr sax, xmlParserInputBufferPtr input,
	      xmlCharEncoding enc) {
    xmlDtdPtr ret = NULL;
    xmlParserCtxtPtr ctxt;
    xmlParserInputPtr pinput = NULL;
    xmlChar start[4];

    if (input == NULL)
	return(NULL);

    ctxt = xmlNewParserCtxt();
    if (ctxt == NULL) {
	return(NULL);
    }

    /*
     * Set-up the SAX context
     */
    if (sax != NULL) { 
	if (ctxt->sax != NULL)
	    xmlFree(ctxt->sax);
        ctxt->sax = sax;
        ctxt->userData = NULL;
    }
    xmlDetectSAX2(ctxt);

    /*
     * generate a parser input from the I/O handler
     */

    pinput = xmlNewIOInputStream(ctxt, input, enc);
    if (pinput == NULL) {
        if (sax != NULL) ctxt->sax = NULL;
	xmlFreeParserCtxt(ctxt);
	return(NULL);
    }

    /*
     * plug some encoding conversion routines here.
     */
    xmlPushInput(ctxt, pinput);

    pinput->filename = NULL;
    pinput->line = 1;
    pinput->col = 1;
    pinput->base = ctxt->input->cur;
    pinput->cur = ctxt->input->cur;
    pinput->free = NULL;

    /*
     * let's parse that entity knowing it's an external subset.
     */
    ctxt->inSubset = 2;
    ctxt->myDoc = xmlNewDoc(BAD_CAST "1.0");
    ctxt->myDoc->extSubset = xmlNewDtd(ctxt->myDoc, BAD_CAST "none",
	                               BAD_CAST "none", BAD_CAST "none");

    if ((enc == XML_CHAR_ENCODING_NONE) &&
        ((ctxt->input->end - ctxt->input->cur) >= 4)) {
	/* 
	 * Get the 4 first bytes and decode the charset
	 * if enc != XML_CHAR_ENCODING_NONE
	 * plug some encoding conversion routines.
	 */
	start[0] = RAW;
	start[1] = NXT(1);
	start[2] = NXT(2);
	start[3] = NXT(3);
	enc = xmlDetectCharEncoding(start, 4);
	if (enc != XML_CHAR_ENCODING_NONE) {
	    xmlSwitchEncoding(ctxt, enc);
	}
    }

    xmlParseExternalSubset(ctxt, BAD_CAST "none", BAD_CAST "none");

    if (ctxt->myDoc != NULL) {
	if (ctxt->wellFormed) {
	    ret = ctxt->myDoc->extSubset;
	    ctxt->myDoc->extSubset = NULL;
	    if (ret != NULL) {
		xmlNodePtr tmp;

		ret->doc = NULL;
		tmp = ret->children;
		while (tmp != NULL) {
		    tmp->doc = NULL;
		    tmp = tmp->next;
		}
	    }
	} else {
	    ret = NULL;
	}
        xmlFreeDoc(ctxt->myDoc);
        ctxt->myDoc = NULL;
    }
    if (sax != NULL) ctxt->sax = NULL;
    xmlFreeParserCtxt(ctxt);
    
    return(ret);
}

/**
 * xmlSAXParseDTD:
 * @sax:  the SAX handler block
 * @ExternalID:  a NAME* containing the External ID of the DTD
 * @SystemID:  a NAME* containing the URL to the DTD
 *
 * Load and parse an external subset.
 * 
 * Returns the resulting xmlDtdPtr or NULL in case of error.
 */

xmlDtdPtr
xmlSAXParseDTD(xmlSAXHandlerPtr sax, const xmlChar *ExternalID,
                          const xmlChar *SystemID) {
    xmlDtdPtr ret = NULL;
    xmlParserCtxtPtr ctxt;
    xmlParserInputPtr input = NULL;
    xmlCharEncoding enc;

    if ((ExternalID == NULL) && (SystemID == NULL)) return(NULL);

    ctxt = xmlNewParserCtxt();
    if (ctxt == NULL) {
	return(NULL);
    }

    /*
     * Set-up the SAX context
     */
    if (sax != NULL) { 
	if (ctxt->sax != NULL)
	    xmlFree(ctxt->sax);
        ctxt->sax = sax;
        ctxt->userData = ctxt;
    }

    /*
     * Ask the Entity resolver to load the damn thing
     */

    if ((ctxt->sax != NULL) && (ctxt->sax->resolveEntity != NULL))
	input = ctxt->sax->resolveEntity(ctxt, ExternalID, SystemID);
    if (input == NULL) {
        if (sax != NULL) ctxt->sax = NULL;
	xmlFreeParserCtxt(ctxt);
	return(NULL);
    }

    /*
     * plug some encoding conversion routines here.
     */
    xmlPushInput(ctxt, input);
    if ((ctxt->input->end - ctxt->input->cur) >= 4) {
	enc = xmlDetectCharEncoding(ctxt->input->cur, 4);
	xmlSwitchEncoding(ctxt, enc);
    }

    if (input->filename == NULL)
	input->filename = (char *) xmlCanonicPath(SystemID);
    input->line = 1;
    input->col = 1;
    input->base = ctxt->input->cur;
    input->cur = ctxt->input->cur;
    input->free = NULL;

    /*
     * let's parse that entity knowing it's an external subset.
     */
    ctxt->inSubset = 2;
    ctxt->myDoc = xmlNewDoc(BAD_CAST "1.0");
    ctxt->myDoc->extSubset = xmlNewDtd(ctxt->myDoc, BAD_CAST "none",
	                               ExternalID, SystemID);
    xmlParseExternalSubset(ctxt, ExternalID, SystemID);

    if (ctxt->myDoc != NULL) {
	if (ctxt->wellFormed) {
	    ret = ctxt->myDoc->extSubset;
	    ctxt->myDoc->extSubset = NULL;
	    if (ret != NULL) {
		xmlNodePtr tmp;

		ret->doc = NULL;
		tmp = ret->children;
		while (tmp != NULL) {
		    tmp->doc = NULL;
		    tmp = tmp->next;
		}
	    }
	} else {
	    ret = NULL;
	}
        xmlFreeDoc(ctxt->myDoc);
        ctxt->myDoc = NULL;
    }
    if (sax != NULL) ctxt->sax = NULL;
    xmlFreeParserCtxt(ctxt);
    
    return(ret);
}


/**
 * xmlParseDTD:
 * @ExternalID:  a NAME* containing the External ID of the DTD
 * @SystemID:  a NAME* containing the URL to the DTD
 *
 * Load and parse an external subset.
 * 
 * Returns the resulting xmlDtdPtr or NULL in case of error.
 */

xmlDtdPtr
xmlParseDTD(const xmlChar *ExternalID, const xmlChar *SystemID) {
    return(xmlSAXParseDTD(NULL, ExternalID, SystemID));
}
#endif /* LIBXML_VALID_ENABLED */

/************************************************************************
 *									*
 * 		Front ends when parsing an Entity			*
 *									*
 ************************************************************************/

/**
 * xmlParseCtxtExternalEntity:
 * @ctx:  the existing parsing context
 * @URL:  the URL for the entity to load
 * @ID:  the System ID for the entity to load
 * @lst:  the return value for the set of parsed nodes
 *
 * Parse an external general entity within an existing parsing context
 * An external general parsed entity is well-formed if it matches the
 * production labeled extParsedEnt.
 *
 * [78] extParsedEnt ::= TextDecl? content
 *
 * Returns 0 if the entity is well formed, -1 in case of args problem and
 *    the parser error code otherwise
 */

int
xmlParseCtxtExternalEntity(xmlParserCtxtPtr ctx, const xmlChar *URL,
	               const xmlChar *ID, xmlNodePtr *lst) {
    xmlParserCtxtPtr ctxt;
    xmlDocPtr newDoc;
    xmlSAXHandlerPtr oldsax = NULL;
    int ret = 0;
    xmlChar start[4];
    xmlCharEncoding enc;

    if (ctx->depth > 40) {
	return(XML_ERR_ENTITY_LOOP);
    }

    if (lst != NULL)
        *lst = NULL;
    if ((URL == NULL) && (ID == NULL))
	return(-1);
    if (ctx->myDoc == NULL) /* @@ relax but check for dereferences */
	return(-1);


    ctxt = xmlCreateEntityParserCtxt(URL, ID, NULL);
    if (ctxt == NULL) return(-1);
    ctxt->userData = ctxt;
    ctxt->_private = ctx->_private;
    oldsax = ctxt->sax;
    ctxt->sax = ctx->sax;
    xmlDetectSAX2(ctxt);
    newDoc = xmlNewDoc(BAD_CAST "1.0");
    if (newDoc == NULL) {
	xmlFreeParserCtxt(ctxt);
	return(-1);
    }
    if (ctx->myDoc != NULL) {
	newDoc->intSubset = ctx->myDoc->intSubset;
	newDoc->extSubset = ctx->myDoc->extSubset;
    }
    if (ctx->myDoc->URL != NULL) {
	newDoc->URL = xmlStrdup(ctx->myDoc->URL);
    }
    newDoc->children = xmlNewDocNode(newDoc, NULL, BAD_CAST "pseudoroot", NULL);
    if (newDoc->children == NULL) {
	ctxt->sax = oldsax;
	xmlFreeParserCtxt(ctxt);
	newDoc->intSubset = NULL;
	newDoc->extSubset = NULL;
        xmlFreeDoc(newDoc);
	return(-1);
    }
    nodePush(ctxt, newDoc->children);
    if (ctx->myDoc == NULL) {
	ctxt->myDoc = newDoc;
    } else {
	ctxt->myDoc = ctx->myDoc;
	newDoc->children->doc = ctx->myDoc;
    }

    /* 
     * Get the 4 first bytes and decode the charset
     * if enc != XML_CHAR_ENCODING_NONE
     * plug some encoding conversion routines.
     */
    GROW
    if ((ctxt->input->end - ctxt->input->cur) >= 4) {
	start[0] = RAW;
	start[1] = NXT(1);
	start[2] = NXT(2);
	start[3] = NXT(3);
	enc = xmlDetectCharEncoding(start, 4);
	if (enc != XML_CHAR_ENCODING_NONE) {
	    xmlSwitchEncoding(ctxt, enc);
	}
    }

    /*
     * Parse a possible text declaration first
     */
    if ((CMP5(CUR_PTR, '<', '?', 'x', 'm', 'l')) && (IS_BLANK_CH(NXT(5)))) {
	xmlParseTextDecl(ctxt);
    }

    /*
     * Doing validity checking on chunk doesn't make sense
     */
    ctxt->instate = XML_PARSER_CONTENT;
    ctxt->validate = ctx->validate;
    ctxt->valid = ctx->valid;
    ctxt->loadsubset = ctx->loadsubset;
    ctxt->depth = ctx->depth + 1;
    ctxt->replaceEntities = ctx->replaceEntities;
    if (ctxt->validate) {
	ctxt->vctxt.error = ctx->vctxt.error;
	ctxt->vctxt.warning = ctx->vctxt.warning;
    } else {
	ctxt->vctxt.error = NULL;
	ctxt->vctxt.warning = NULL;
    }
    ctxt->vctxt.nodeTab = NULL;
    ctxt->vctxt.nodeNr = 0;
    ctxt->vctxt.nodeMax = 0;
    ctxt->vctxt.node = NULL;

    xmlParseContent(ctxt);
   
    ctx->validate = ctxt->validate;
    ctx->valid = ctxt->valid;
    if ((RAW == '<') && (NXT(1) == '/')) {
	xmlFatalErr(ctxt, XML_ERR_NOT_WELL_BALANCED, NULL);
    } else if (RAW != 0) {
	xmlFatalErr(ctxt, XML_ERR_EXTRA_CONTENT, NULL);
    }
    if (ctxt->node != newDoc->children) {
	xmlFatalErr(ctxt, XML_ERR_NOT_WELL_BALANCED, NULL);
    }

    if (!ctxt->wellFormed) {
        if (ctxt->errNo == 0)
	    ret = 1;
	else
	    ret = ctxt->errNo;
    } else {
	if (lst != NULL) {
	    xmlNodePtr cur;

	    /*
	     * Return the newly created nodeset after unlinking it from
	     * they pseudo parent.
	     */
	    cur = newDoc->children->children;
	    *lst = cur;
	    while (cur != NULL) {
		cur->parent = NULL;
		cur = cur->next;
	    }
            newDoc->children->children = NULL;
	}
	ret = 0;
    }
    ctxt->sax = oldsax;
    xmlFreeParserCtxt(ctxt);
    newDoc->intSubset = NULL;
    newDoc->extSubset = NULL;
    xmlFreeDoc(newDoc);
    
    return(ret);
}

/**
 * xmlParseExternalEntityPrivate:
 * @doc:  the document the chunk pertains to
 * @oldctxt:  the previous parser context if available
 * @sax:  the SAX handler bloc (possibly NULL)
 * @user_data:  The user data returned on SAX callbacks (possibly NULL)
 * @depth:  Used for loop detection, use 0
 * @URL:  the URL for the entity to load
 * @ID:  the System ID for the entity to load
 * @list:  the return value for the set of parsed nodes
 *
 * Private version of xmlParseExternalEntity()
 *
 * Returns 0 if the entity is well formed, -1 in case of args problem and
 *    the parser error code otherwise
 */

static xmlParserErrors
xmlParseExternalEntityPrivate(xmlDocPtr doc, xmlParserCtxtPtr oldctxt,
	              xmlSAXHandlerPtr sax,
		      void *user_data, int depth, const xmlChar *URL,
		      const xmlChar *ID, xmlNodePtr *list) {
    xmlParserCtxtPtr ctxt;
    xmlDocPtr newDoc;
    xmlSAXHandlerPtr oldsax = NULL;
    xmlParserErrors ret = XML_ERR_OK;
    xmlChar start[4];
    xmlCharEncoding enc;

    if (depth > 40) {
	return(XML_ERR_ENTITY_LOOP);
    }



    if (list != NULL)
        *list = NULL;
    if ((URL == NULL) && (ID == NULL))
	return(XML_ERR_INTERNAL_ERROR);
    if (doc == NULL) /* @@ relax but check for dereferences */
	return(XML_ERR_INTERNAL_ERROR);


    ctxt = xmlCreateEntityParserCtxt(URL, ID, NULL);
    if (ctxt == NULL) return(XML_WAR_UNDECLARED_ENTITY);
    ctxt->userData = ctxt;
    if (oldctxt != NULL) {
	ctxt->_private = oldctxt->_private;
	ctxt->loadsubset = oldctxt->loadsubset;
	ctxt->validate = oldctxt->validate;
	ctxt->external = oldctxt->external;
	ctxt->record_info = oldctxt->record_info;
	ctxt->node_seq.maximum = oldctxt->node_seq.maximum;
	ctxt->node_seq.length = oldctxt->node_seq.length;
	ctxt->node_seq.buffer = oldctxt->node_seq.buffer;
    } else {
	/*
	 * Doing validity checking on chunk without context
	 * doesn't make sense
	 */
	ctxt->_private = NULL;
	ctxt->validate = 0;
	ctxt->external = 2;
	ctxt->loadsubset = 0;
    }
    if (sax != NULL) {
	oldsax = ctxt->sax;
        ctxt->sax = sax;
	if (user_data != NULL)
	    ctxt->userData = user_data;
    }
    xmlDetectSAX2(ctxt);
    newDoc = xmlNewDoc(BAD_CAST "1.0");
    if (newDoc == NULL) {
	ctxt->node_seq.maximum = 0;
	ctxt->node_seq.length = 0;
	ctxt->node_seq.buffer = NULL;
	xmlFreeParserCtxt(ctxt);
	return(XML_ERR_INTERNAL_ERROR);
    }
    if (doc != NULL) {
	newDoc->intSubset = doc->intSubset;
	newDoc->extSubset = doc->extSubset;
    }
    if (doc->URL != NULL) {
	newDoc->URL = xmlStrdup(doc->URL);
    }
    newDoc->children = xmlNewDocNode(newDoc, NULL, BAD_CAST "pseudoroot", NULL);
    if (newDoc->children == NULL) {
	if (sax != NULL)
	    ctxt->sax = oldsax;
	ctxt->node_seq.maximum = 0;
	ctxt->node_seq.length = 0;
	ctxt->node_seq.buffer = NULL;
	xmlFreeParserCtxt(ctxt);
	newDoc->intSubset = NULL;
	newDoc->extSubset = NULL;
        xmlFreeDoc(newDoc);
	return(XML_ERR_INTERNAL_ERROR);
    }
    nodePush(ctxt, newDoc->children);
    if (doc == NULL) {
	ctxt->myDoc = newDoc;
    } else {
	ctxt->myDoc = doc;
	newDoc->children->doc = doc;
    }

    /* 
     * Get the 4 first bytes and decode the charset
     * if enc != XML_CHAR_ENCODING_NONE
     * plug some encoding conversion routines.
     */
    GROW;
    if ((ctxt->input->end - ctxt->input->cur) >= 4) {
	start[0] = RAW;
	start[1] = NXT(1);
	start[2] = NXT(2);
	start[3] = NXT(3);
	enc = xmlDetectCharEncoding(start, 4);
	if (enc != XML_CHAR_ENCODING_NONE) {
	    xmlSwitchEncoding(ctxt, enc);
	}
    }

    /*
     * Parse a possible text declaration first
     */
    if ((CMP5(CUR_PTR, '<', '?', 'x', 'm', 'l')) && (IS_BLANK_CH(NXT(5)))) {
	xmlParseTextDecl(ctxt);
    }

    ctxt->instate = XML_PARSER_CONTENT;
    ctxt->depth = depth;

    xmlParseContent(ctxt);
   
    if ((RAW == '<') && (NXT(1) == '/')) {
	xmlFatalErr(ctxt, XML_ERR_NOT_WELL_BALANCED, NULL);
    } else if (RAW != 0) {
	xmlFatalErr(ctxt, XML_ERR_EXTRA_CONTENT, NULL);
    }
    if (ctxt->node != newDoc->children) {
	xmlFatalErr(ctxt, XML_ERR_NOT_WELL_BALANCED, NULL);
    }

    if (!ctxt->wellFormed) {
        if (ctxt->errNo == 0)
	    ret = XML_ERR_INTERNAL_ERROR;
	else
	    ret = (xmlParserErrors)ctxt->errNo;
    } else {
	if (list != NULL) {
	    xmlNodePtr cur;

	    /*
	     * Return the newly created nodeset after unlinking it from
	     * they pseudo parent.
	     */
	    cur = newDoc->children->children;
	    *list = cur;
	    while (cur != NULL) {
		cur->parent = NULL;
		cur = cur->next;
	    }
            newDoc->children->children = NULL;
	}
	ret = XML_ERR_OK;
    }
    if (sax != NULL) 
	ctxt->sax = oldsax;
    oldctxt->node_seq.maximum = ctxt->node_seq.maximum;
    oldctxt->node_seq.length = ctxt->node_seq.length;
    oldctxt->node_seq.buffer = ctxt->node_seq.buffer;
    ctxt->node_seq.maximum = 0;
    ctxt->node_seq.length = 0;
    ctxt->node_seq.buffer = NULL;
    xmlFreeParserCtxt(ctxt);
    newDoc->intSubset = NULL;
    newDoc->extSubset = NULL;
    xmlFreeDoc(newDoc);
    
    return(ret);
}

#ifdef LIBXML_SAX1_ENABLED
/**
 * xmlParseExternalEntity:
 * @doc:  the document the chunk pertains to
 * @sax:  the SAX handler bloc (possibly NULL)
 * @user_data:  The user data returned on SAX callbacks (possibly NULL)
 * @depth:  Used for loop detection, use 0
 * @URL:  the URL for the entity to load
 * @ID:  the System ID for the entity to load
 * @lst:  the return value for the set of parsed nodes
 *
 * Parse an external general entity
 * An external general parsed entity is well-formed if it matches the
 * production labeled extParsedEnt.
 *
 * [78] extParsedEnt ::= TextDecl? content
 *
 * Returns 0 if the entity is well formed, -1 in case of args problem and
 *    the parser error code otherwise
 */

int
xmlParseExternalEntity(xmlDocPtr doc, xmlSAXHandlerPtr sax, void *user_data,
	  int depth, const xmlChar *URL, const xmlChar *ID, xmlNodePtr *lst) {
    return(xmlParseExternalEntityPrivate(doc, NULL, sax, user_data, depth, URL,
		                       ID, lst));
}

/**
 * xmlParseBalancedChunkMemory:
 * @doc:  the document the chunk pertains to
 * @sax:  the SAX handler bloc (possibly NULL)
 * @user_data:  The user data returned on SAX callbacks (possibly NULL)
 * @depth:  Used for loop detection, use 0
 * @string:  the input string in UTF8 or ISO-Latin (zero terminated)
 * @lst:  the return value for the set of parsed nodes
 *
 * Parse a well-balanced chunk of an XML document
 * called by the parser
 * The allowed sequence for the Well Balanced Chunk is the one defined by
 * the content production in the XML grammar:
 *
 * [43] content ::= (element | CharData | Reference | CDSect | PI | Comment)*
 *
 * Returns 0 if the chunk is well balanced, -1 in case of args problem and
 *    the parser error code otherwise
 */

int
xmlParseBalancedChunkMemory(xmlDocPtr doc, xmlSAXHandlerPtr sax,
     void *user_data, int depth, const xmlChar *string, xmlNodePtr *lst) {
    return xmlParseBalancedChunkMemoryRecover( doc, sax, user_data,
                                                depth, string, lst, 0 );
}
#endif /* LIBXML_SAX1_ENABLED */

/**
 * xmlParseBalancedChunkMemoryInternal:
 * @oldctxt:  the existing parsing context
 * @string:  the input string in UTF8 or ISO-Latin (zero terminated)
 * @user_data:  the user data field for the parser context
 * @lst:  the return value for the set of parsed nodes
 *
 *
 * Parse a well-balanced chunk of an XML document
 * called by the parser
 * The allowed sequence for the Well Balanced Chunk is the one defined by
 * the content production in the XML grammar:
 *
 * [43] content ::= (element | CharData | Reference | CDSect | PI | Comment)*
 *
 * Returns XML_ERR_OK if the chunk is well balanced, and the parser
 * error code otherwise
 *    
 * In case recover is set to 1, the nodelist will not be empty even if
 * the parsed chunk is not well balanced. 
 */
static xmlParserErrors
xmlParseBalancedChunkMemoryInternal(xmlParserCtxtPtr oldctxt,
	const xmlChar *string, void *user_data, xmlNodePtr *lst) {
    xmlParserCtxtPtr ctxt;
    xmlDocPtr newDoc = NULL;
    xmlSAXHandlerPtr oldsax = NULL;
    xmlNodePtr content = NULL;
    int size;
    xmlParserErrors ret = XML_ERR_OK;

    if (oldctxt->depth > 40) {
	return(XML_ERR_ENTITY_LOOP);
    }


    if (lst != NULL)
        *lst = NULL;
    if (string == NULL)
        return(XML_ERR_INTERNAL_ERROR);

    size = xmlStrlen(string);

    ctxt = xmlCreateMemoryParserCtxt((char *) string, size);
    if (ctxt == NULL) return(XML_WAR_UNDECLARED_ENTITY);
    if (user_data != NULL)
	ctxt->userData = user_data;
    else
	ctxt->userData = ctxt;
    if (ctxt->dict != NULL) xmlDictFree(ctxt->dict);
    ctxt->dict = oldctxt->dict;

    oldsax = ctxt->sax;
    ctxt->sax = oldctxt->sax;
    xmlDetectSAX2(ctxt);
    
    ctxt->_private = oldctxt->_private;
    if (oldctxt->myDoc == NULL) {
	newDoc = xmlNewDoc(BAD_CAST "1.0");
	if (newDoc == NULL) {
	    ctxt->sax = oldsax;
	    ctxt->dict = NULL;
	    xmlFreeParserCtxt(ctxt);
	    return(XML_ERR_INTERNAL_ERROR);
	}
	ctxt->myDoc = newDoc;
    } else {
	ctxt->myDoc = oldctxt->myDoc;
        content = ctxt->myDoc->children;
    }
    ctxt->myDoc->children = xmlNewDocNode(ctxt->myDoc, NULL,
	                                  BAD_CAST "pseudoroot", NULL);
    if (ctxt->myDoc->children == NULL) {
	ctxt->sax = oldsax;
	ctxt->dict = NULL;
	xmlFreeParserCtxt(ctxt);
	if (newDoc != NULL)
	    xmlFreeDoc(newDoc);
	return(XML_ERR_INTERNAL_ERROR);
    }
    nodePush(ctxt, ctxt->myDoc->children);
    ctxt->instate = XML_PARSER_CONTENT;
    ctxt->depth = oldctxt->depth + 1;

    ctxt->validate = 0;
    ctxt->loadsubset = oldctxt->loadsubset;
    if ((oldctxt->validate) || (oldctxt->replaceEntities != 0)) {
	/*
	 * ID/IDREF registration will be done in xmlValidateElement below
	 */
	ctxt->loadsubset |= XML_SKIP_IDS;
    }
    ctxt->dictNames = oldctxt->dictNames;

    xmlParseContent(ctxt);
    if ((RAW == '<') && (NXT(1) == '/')) {
	xmlFatalErr(ctxt, XML_ERR_NOT_WELL_BALANCED, NULL);
    } else if (RAW != 0) {
	xmlFatalErr(ctxt, XML_ERR_EXTRA_CONTENT, NULL);
    }
    if (ctxt->node != ctxt->myDoc->children) {
	xmlFatalErr(ctxt, XML_ERR_NOT_WELL_BALANCED, NULL);
    }

    if (!ctxt->wellFormed) {
        if (ctxt->errNo == 0)
	    ret = XML_ERR_INTERNAL_ERROR;
	else
	    ret = (xmlParserErrors)ctxt->errNo;
    } else {
      ret = XML_ERR_OK;
    }
    
    if ((lst != NULL) && (ret == XML_ERR_OK)) {
	xmlNodePtr cur;

	/*
	 * Return the newly created nodeset after unlinking it from
	 * they pseudo parent.
	 */
	cur = ctxt->myDoc->children->children;
	*lst = cur;
	while (cur != NULL) {
#ifdef LIBXML_VALID_ENABLED
	    if (oldctxt->validate && oldctxt->wellFormed &&
		oldctxt->myDoc && oldctxt->myDoc->intSubset) {
		oldctxt->valid &= xmlValidateElement(&oldctxt->vctxt,
			oldctxt->myDoc, cur);
	    }
#endif /* LIBXML_VALID_ENABLED */
	    cur->parent = NULL;
	    cur = cur->next;
	}
	ctxt->myDoc->children->children = NULL;
    }
    if (ctxt->myDoc != NULL) {
	xmlFreeNode(ctxt->myDoc->children);
        ctxt->myDoc->children = content;
    }
	
    ctxt->sax = oldsax;
    ctxt->dict = NULL;
    xmlFreeParserCtxt(ctxt);
    if (newDoc != NULL)
	xmlFreeDoc(newDoc);
    
    return(ret);
}

#ifdef LIBXML_SAX1_ENABLED
/**
 * xmlParseBalancedChunkMemoryRecover:
 * @doc:  the document the chunk pertains to
 * @sax:  the SAX handler bloc (possibly NULL)
 * @user_data:  The user data returned on SAX callbacks (possibly NULL)
 * @depth:  Used for loop detection, use 0
 * @string:  the input string in UTF8 or ISO-Latin (zero terminated)
 * @lst:  the return value for the set of parsed nodes
 * @recover: return nodes even if the data is broken (use 0)
 *
 *
 * Parse a well-balanced chunk of an XML document
 * called by the parser
 * The allowed sequence for the Well Balanced Chunk is the one defined by
 * the content production in the XML grammar:
 *
 * [43] content ::= (element | CharData | Reference | CDSect | PI | Comment)*
 *
 * Returns 0 if the chunk is well balanced, -1 in case of args problem and
 *    the parser error code otherwise
 *    
 * In case recover is set to 1, the nodelist will not be empty even if
 * the parsed chunk is not well balanced. 
 */
int
xmlParseBalancedChunkMemoryRecover(xmlDocPtr doc, xmlSAXHandlerPtr sax,
     void *user_data, int depth, const xmlChar *string, xmlNodePtr *lst, 
     int recover) {
    xmlParserCtxtPtr ctxt;
    xmlDocPtr newDoc;
    xmlSAXHandlerPtr oldsax = NULL;
    xmlNodePtr content;
    int size;
    int ret = 0;

    if (depth > 40) {
	return(XML_ERR_ENTITY_LOOP);
    }


    if (lst != NULL)
        *lst = NULL;
    if (string == NULL)
        return(-1);

    size = xmlStrlen(string);

    ctxt = xmlCreateMemoryParserCtxt((char *) string, size);
    if (ctxt == NULL) return(-1);
    ctxt->userData = ctxt;
    if (sax != NULL) {
	oldsax = ctxt->sax;
        ctxt->sax = sax;
	if (user_data != NULL)
	    ctxt->userData = user_data;
    }
    newDoc = xmlNewDoc(BAD_CAST "1.0");
    if (newDoc == NULL) {
	xmlFreeParserCtxt(ctxt);
	return(-1);
    }
    if (doc != NULL) {
	newDoc->intSubset = doc->intSubset;
	newDoc->extSubset = doc->extSubset;
    }
    newDoc->children = xmlNewDocNode(newDoc, NULL, BAD_CAST "pseudoroot", NULL);
    if (newDoc->children == NULL) {
	if (sax != NULL)
	    ctxt->sax = oldsax;
	xmlFreeParserCtxt(ctxt);
	newDoc->intSubset = NULL;
	newDoc->extSubset = NULL;
        xmlFreeDoc(newDoc);
	return(-1);
    }
    nodePush(ctxt, newDoc->children);
    if (doc == NULL) {
	ctxt->myDoc = newDoc;
    } else {
	ctxt->myDoc = newDoc;
	newDoc->children->doc = doc;
    }
    ctxt->instate = XML_PARSER_CONTENT;
    ctxt->depth = depth;

    /*
     * Doing validity checking on chunk doesn't make sense
     */
    ctxt->validate = 0;
    ctxt->loadsubset = 0;
    xmlDetectSAX2(ctxt);

    if ( doc != NULL ){
        content = doc->children;
        doc->children = NULL;
        xmlParseContent(ctxt);
        doc->children = content;
    }
    else {
        xmlParseContent(ctxt);
    }
    if ((RAW == '<') && (NXT(1) == '/')) {
	xmlFatalErr(ctxt, XML_ERR_NOT_WELL_BALANCED, NULL);
    } else if (RAW != 0) {
	xmlFatalErr(ctxt, XML_ERR_EXTRA_CONTENT, NULL);
    }
    if (ctxt->node != newDoc->children) {
	xmlFatalErr(ctxt, XML_ERR_NOT_WELL_BALANCED, NULL);
    }

    if (!ctxt->wellFormed) {
        if (ctxt->errNo == 0)
	    ret = 1;
	else
	    ret = ctxt->errNo;
    } else {
      ret = 0;
    }
    
    if (lst != NULL && (ret == 0 || recover == 1)) {
      xmlNodePtr cur;

	    /*
	     * Return the newly created nodeset after unlinking it from
	     * they pseudo parent.
	     */
	    cur = newDoc->children->children;
	    *lst = cur;
	    while (cur != NULL) {
		cur->parent = NULL;
		cur = cur->next;
	    }
            newDoc->children->children = NULL;
	}
	
    if (sax != NULL) 
	ctxt->sax = oldsax;
    xmlFreeParserCtxt(ctxt);
    newDoc->intSubset = NULL;
    newDoc->extSubset = NULL;
    xmlFreeDoc(newDoc);
    
    return(ret);
}

/**
 * xmlSAXParseEntity:
 * @sax:  the SAX handler block
 * @filename:  the filename
 *
 * parse an XML external entity out of context and build a tree.
 * It use the given SAX function block to handle the parsing callback.
 * If sax is NULL, fallback to the default DOM tree building routines.
 *
 * [78] extParsedEnt ::= TextDecl? content
 *
 * This correspond to a "Well Balanced" chunk
 *
 * Returns the resulting document tree
 */

xmlDocPtr
xmlSAXParseEntity(xmlSAXHandlerPtr sax, const char *filename) {
    xmlDocPtr ret;
    xmlParserCtxtPtr ctxt;

    ctxt = xmlCreateFileParserCtxt(filename);
    if (ctxt == NULL) {
	return(NULL);
    }
    if (sax != NULL) {
	if (ctxt->sax != NULL)
	    xmlFree(ctxt->sax);
        ctxt->sax = sax;
        ctxt->userData = NULL;
    }

    xmlParseExtParsedEnt(ctxt);

    if (ctxt->wellFormed)
	ret = ctxt->myDoc;
    else {
        ret = NULL;
        xmlFreeDoc(ctxt->myDoc);
        ctxt->myDoc = NULL;
    }
    if (sax != NULL)
        ctxt->sax = NULL;
    xmlFreeParserCtxt(ctxt);
    
    return(ret);
}

/**
 * xmlParseEntity:
 * @filename:  the filename
 *
 * parse an XML external entity out of context and build a tree.
 *
 * [78] extParsedEnt ::= TextDecl? content
 *
 * This correspond to a "Well Balanced" chunk
 *
 * Returns the resulting document tree
 */

xmlDocPtr
xmlParseEntity(const char *filename) {
    return(xmlSAXParseEntity(NULL, filename));
}
#endif /* LIBXML_SAX1_ENABLED */

/**
 * xmlCreateEntityParserCtxt:
 * @URL:  the entity URL
 * @ID:  the entity PUBLIC ID
 * @base:  a possible base for the target URI
 *
 * Create a parser context for an external entity
 * Automatic support for ZLIB/Compress compressed document is provided
 * by default if found at compile-time.
 *
 * Returns the new parser context or NULL
 */
xmlParserCtxtPtr
xmlCreateEntityParserCtxt(const xmlChar *URL, const xmlChar *ID,
	                  const xmlChar *base) {
    xmlParserCtxtPtr ctxt;
    xmlParserInputPtr inputStream;
    char *directory = NULL;
    xmlChar *uri;
    
    ctxt = xmlNewParserCtxt();
    if (ctxt == NULL) {
	return(NULL);
    }

    uri = xmlBuildURI(URL, base);

    if (uri == NULL) {
	inputStream = xmlLoadExternalEntity((char *)URL, (char *)ID, ctxt);
	if (inputStream == NULL) {
	    xmlFreeParserCtxt(ctxt);
	    return(NULL);
	}

	inputPush(ctxt, inputStream);

	if ((ctxt->directory == NULL) && (directory == NULL))
	    directory = xmlParserGetDirectory((char *)URL);
	if ((ctxt->directory == NULL) && (directory != NULL))
	    ctxt->directory = directory;
    } else {
	inputStream = xmlLoadExternalEntity((char *)uri, (char *)ID, ctxt);
	if (inputStream == NULL) {
	    xmlFree(uri);
	    xmlFreeParserCtxt(ctxt);
	    return(NULL);
	}

	inputPush(ctxt, inputStream);

	if ((ctxt->directory == NULL) && (directory == NULL))
	    directory = xmlParserGetDirectory((char *)uri);
	if ((ctxt->directory == NULL) && (directory != NULL))
	    ctxt->directory = directory;
	xmlFree(uri);
    }
    return(ctxt);
}

/************************************************************************
 *									*
 * 		Front ends when parsing from a file			*
 *									*
 ************************************************************************/

/**
 * xmlCreateFileParserCtxt:
 * @filename:  the filename
 *
 * Create a parser context for a file content. 
 * Automatic support for ZLIB/Compress compressed document is provided
 * by default if found at compile-time.
 *
 * Returns the new parser context or NULL
 */
xmlParserCtxtPtr
xmlCreateFileParserCtxt(const char *filename)
{
    xmlParserCtxtPtr ctxt;
    xmlParserInputPtr inputStream;
    char *directory = NULL;

    ctxt = xmlNewParserCtxt();
    if (ctxt == NULL) {
	xmlErrMemory(NULL, "cannot allocate parser context");
	return(NULL);
    }

    
    inputStream = xmlLoadExternalEntity(filename, NULL, ctxt);
    if (inputStream == NULL) {
	xmlFreeParserCtxt(ctxt);
	return(NULL);
    }

    inputPush(ctxt, inputStream);
    if ((ctxt->directory == NULL) && (directory == NULL))
        directory = xmlParserGetDirectory(filename);
    if ((ctxt->directory == NULL) && (directory != NULL))
        ctxt->directory = directory;

    return(ctxt);
}

#ifdef LIBXML_SAX1_ENABLED
/**
 * xmlSAXParseFileWithData:
 * @sax:  the SAX handler block
 * @filename:  the filename
 * @recovery:  work in recovery mode, i.e. tries to read no Well Formed
 *             documents
 * @data:  the userdata
 *
 * parse an XML file and build a tree. Automatic support for ZLIB/Compress
 * compressed document is provided by default if found at compile-time.
 * It use the given SAX function block to handle the parsing callback.
 * If sax is NULL, fallback to the default DOM tree building routines.
 *
 * User data (void *) is stored within the parser context in the
 * context's _private member, so it is available nearly everywhere in libxml
 *
 * Returns the resulting document tree
 */

xmlDocPtr
xmlSAXParseFileWithData(xmlSAXHandlerPtr sax, const char *filename,
                        int recovery, void *data) {
    xmlDocPtr ret;
    xmlParserCtxtPtr ctxt;
    char *directory = NULL;

    xmlInitParser();

    ctxt = xmlCreateFileParserCtxt(filename);
    if (ctxt == NULL) {
	return(NULL);
    }
    if (sax != NULL) {
	if (ctxt->sax != NULL)
	    xmlFree(ctxt->sax);
        ctxt->sax = sax;
    }
    xmlDetectSAX2(ctxt);
    if (data!=NULL) {
	ctxt->_private = data;
    }

    if ((ctxt->directory == NULL) && (directory == NULL))
        directory = xmlParserGetDirectory(filename);
    if ((ctxt->directory == NULL) && (directory != NULL))
        ctxt->directory = (char *) xmlStrdup((xmlChar *) directory);

    ctxt->recovery = recovery;

    xmlParseDocument(ctxt);

    if ((ctxt->wellFormed) || recovery) {
        ret = ctxt->myDoc;
	if (ret != NULL) {
	    if (ctxt->input->buf->compressed > 0)
		ret->compression = 9;
	    else
		ret->compression = ctxt->input->buf->compressed;
	}
    }
    else {
       ret = NULL;
       xmlFreeDoc(ctxt->myDoc);
       ctxt->myDoc = NULL;
    }
    if (sax != NULL)
        ctxt->sax = NULL;
    xmlFreeParserCtxt(ctxt);
    
    return(ret);
}

/**
 * xmlSAXParseFile:
 * @sax:  the SAX handler block
 * @filename:  the filename
 * @recovery:  work in recovery mode, i.e. tries to read no Well Formed
 *             documents
 *
 * parse an XML file and build a tree. Automatic support for ZLIB/Compress
 * compressed document is provided by default if found at compile-time.
 * It use the given SAX function block to handle the parsing callback.
 * If sax is NULL, fallback to the default DOM tree building routines.
 *
 * Returns the resulting document tree
 */

xmlDocPtr
xmlSAXParseFile(xmlSAXHandlerPtr sax, const char *filename,
                          int recovery) {
    return(xmlSAXParseFileWithData(sax,filename,recovery,NULL));
}

/**
 * xmlRecoverDoc:
 * @cur:  a pointer to an array of xmlChar
 *
 * parse an XML in-memory document and build a tree.
 * In the case the document is not Well Formed, a tree is built anyway
 * 
 * Returns the resulting document tree
 */

xmlDocPtr
xmlRecoverDoc(xmlChar *cur) {
    return(xmlSAXParseDoc(NULL, cur, 1));
}

/**
 * xmlParseFile:
 * @filename:  the filename
 *
 * parse an XML file and build a tree. Automatic support for ZLIB/Compress
 * compressed document is provided by default if found at compile-time.
 *
 * Returns the resulting document tree if the file was wellformed,
 * NULL otherwise.
 */

xmlDocPtr
xmlParseFile(const char *filename) {
    return(xmlSAXParseFile(NULL, filename, 0));
}

/**
 * xmlRecoverFile:
 * @filename:  the filename
 *
 * parse an XML file and build a tree. Automatic support for ZLIB/Compress
 * compressed document is provided by default if found at compile-time.
 * In the case the document is not Well Formed, a tree is built anyway
 *
 * Returns the resulting document tree
 */

xmlDocPtr
xmlRecoverFile(const char *filename) {
    return(xmlSAXParseFile(NULL, filename, 1));
}


/**
 * xmlSetupParserForBuffer:
 * @ctxt:  an XML parser context
 * @buffer:  a xmlChar * buffer
 * @filename:  a file name
 *
 * Setup the parser context to parse a new buffer; Clears any prior
 * contents from the parser context. The buffer parameter must not be
 * NULL, but the filename parameter can be
 */
void
xmlSetupParserForBuffer(xmlParserCtxtPtr ctxt, const xmlChar* buffer,
                             const char* filename)
{
    xmlParserInputPtr input;

    input = xmlNewInputStream(ctxt);
    if (input == NULL) {
        xmlErrMemory(NULL, "parsing new buffer: out of memory\n");
        xmlFree(ctxt);
        return;
    }
  
    xmlClearParserCtxt(ctxt);
    if (filename != NULL)
        input->filename = (char *) xmlCanonicPath((const xmlChar *)filename);
    input->base = buffer;
    input->cur = buffer;
    input->end = &buffer[xmlStrlen(buffer)];
    inputPush(ctxt, input);
}

/**
 * xmlSAXUserParseFile:
 * @sax:  a SAX handler
 * @user_data:  The user data returned on SAX callbacks
 * @filename:  a file name
 *
 * parse an XML file and call the given SAX handler routines.
 * Automatic support for ZLIB/Compress compressed document is provided
 * 
 * Returns 0 in case of success or a error number otherwise
 */
int
xmlSAXUserParseFile(xmlSAXHandlerPtr sax, void *user_data,
                    const char *filename) {
    int ret = 0;
    xmlParserCtxtPtr ctxt;
    
    ctxt = xmlCreateFileParserCtxt(filename);
    if (ctxt == NULL) return -1;
#ifdef LIBXML_SAX1_ENABLED
    if (ctxt->sax != (xmlSAXHandlerPtr) &xmlDefaultSAXHandler)
#endif /* LIBXML_SAX1_ENABLED */
	xmlFree(ctxt->sax);
    ctxt->sax = sax;
    xmlDetectSAX2(ctxt);

    if (user_data != NULL)
	ctxt->userData = user_data;
    
    xmlParseDocument(ctxt);
    
    if (ctxt->wellFormed)
	ret = 0;
    else {
        if (ctxt->errNo != 0)
	    ret = ctxt->errNo;
	else
	    ret = -1;
    }
    if (sax != NULL)
	ctxt->sax = NULL;
    xmlFreeParserCtxt(ctxt);
    
    return ret;
}
#endif /* LIBXML_SAX1_ENABLED */

/************************************************************************
 *									*
 * 		Front ends when parsing from memory			*
 *									*
 ************************************************************************/

/**
 * xmlCreateMemoryParserCtxt:
 * @buffer:  a pointer to a char array
 * @size:  the size of the array
 *
 * Create a parser context for an XML in-memory document.
 *
 * Returns the new parser context or NULL
 */
xmlParserCtxtPtr
xmlCreateMemoryParserCtxt(const char *buffer, int size) {
    xmlParserCtxtPtr ctxt;
    xmlParserInputPtr input;
    xmlParserInputBufferPtr buf;

    if (buffer == NULL)
	return(NULL);
    if (size <= 0)
	return(NULL);

    ctxt = xmlNewParserCtxt();
    if (ctxt == NULL)
	return(NULL);

    /* TODO: xmlParserInputBufferCreateStatic, requires some serious changes */
    buf = xmlParserInputBufferCreateMem(buffer, size, XML_CHAR_ENCODING_NONE);
    if (buf == NULL) {
	xmlFreeParserCtxt(ctxt);
	return(NULL);
    }

    input = xmlNewInputStream(ctxt);
    if (input == NULL) {
	xmlFreeParserInputBuffer(buf);
	xmlFreeParserCtxt(ctxt);
	return(NULL);
    }

    input->filename = NULL;
    input->buf = buf;
    input->base = input->buf->buffer->content;
    input->cur = input->buf->buffer->content;
    input->end = &input->buf->buffer->content[input->buf->buffer->use];

    inputPush(ctxt, input);
    return(ctxt);
}

#ifdef LIBXML_SAX1_ENABLED
/**
 * xmlSAXParseMemoryWithData:
 * @sax:  the SAX handler block
 * @buffer:  an pointer to a char array
 * @size:  the size of the array
 * @recovery:  work in recovery mode, i.e. tries to read no Well Formed
 *             documents
 * @data:  the userdata
 *
 * parse an XML in-memory block and use the given SAX function block
 * to handle the parsing callback. If sax is NULL, fallback to the default
 * DOM tree building routines.
 *
 * User data (void *) is stored within the parser context in the
 * context's _private member, so it is available nearly everywhere in libxml
 *
 * Returns the resulting document tree
 */

xmlDocPtr
xmlSAXParseMemoryWithData(xmlSAXHandlerPtr sax, const char *buffer,
	          int size, int recovery, void *data) {
    xmlDocPtr ret;
    xmlParserCtxtPtr ctxt;

    ctxt = xmlCreateMemoryParserCtxt(buffer, size);
    if (ctxt == NULL) return(NULL);
    if (sax != NULL) {
	if (ctxt->sax != NULL)
	    xmlFree(ctxt->sax);
        ctxt->sax = sax;
    }
    xmlDetectSAX2(ctxt);
    if (data!=NULL) {
	ctxt->_private=data;
    }

    ctxt->recovery = recovery;

    xmlParseDocument(ctxt);

    if ((ctxt->wellFormed) || recovery) ret = ctxt->myDoc;
    else {
       ret = NULL;
       xmlFreeDoc(ctxt->myDoc);
       ctxt->myDoc = NULL;
    }
    if (sax != NULL) 
	ctxt->sax = NULL;
    xmlFreeParserCtxt(ctxt);
    
    return(ret);
}

/**
 * xmlSAXParseMemory:
 * @sax:  the SAX handler block
 * @buffer:  an pointer to a char array
 * @size:  the size of the array
 * @recovery:  work in recovery mode, i.e. tries to read not Well Formed
 *             documents
 *
 * parse an XML in-memory block and use the given SAX function block
 * to handle the parsing callback. If sax is NULL, fallback to the default
 * DOM tree building routines.
 * 
 * Returns the resulting document tree
 */
xmlDocPtr
xmlSAXParseMemory(xmlSAXHandlerPtr sax, const char *buffer,
	          int size, int recovery) {
    return xmlSAXParseMemoryWithData(sax, buffer, size, recovery, NULL);
}

/**
 * xmlParseMemory:
 * @buffer:  an pointer to a char array
 * @size:  the size of the array
 *
 * parse an XML in-memory block and build a tree.
 * 
 * Returns the resulting document tree
 */

xmlDocPtr xmlParseMemory(const char *buffer, int size) {
   return(xmlSAXParseMemory(NULL, buffer, size, 0));
}

/**
 * xmlRecoverMemory:
 * @buffer:  an pointer to a char array
 * @size:  the size of the array
 *
 * parse an XML in-memory block and build a tree.
 * In the case the document is not Well Formed, a tree is built anyway
 * 
 * Returns the resulting document tree
 */

xmlDocPtr xmlRecoverMemory(const char *buffer, int size) {
   return(xmlSAXParseMemory(NULL, buffer, size, 1));
}

/**
 * xmlSAXUserParseMemory:
 * @sax:  a SAX handler
 * @user_data:  The user data returned on SAX callbacks
 * @buffer:  an in-memory XML document input
 * @size:  the length of the XML document in bytes
 *
 * A better SAX parsing routine.
 * parse an XML in-memory buffer and call the given SAX handler routines.
 * 
 * Returns 0 in case of success or a error number otherwise
 */
int xmlSAXUserParseMemory(xmlSAXHandlerPtr sax, void *user_data,
			  const char *buffer, int size) {
    int ret = 0;
    xmlParserCtxtPtr ctxt;
    xmlSAXHandlerPtr oldsax = NULL;
    
    if (sax == NULL) return -1;
    ctxt = xmlCreateMemoryParserCtxt(buffer, size);
    if (ctxt == NULL) return -1;
    oldsax = ctxt->sax;
    ctxt->sax = sax;
    xmlDetectSAX2(ctxt);
    if (user_data != NULL)
	ctxt->userData = user_data;
    
    xmlParseDocument(ctxt);
    
    if (ctxt->wellFormed)
	ret = 0;
    else {
        if (ctxt->errNo != 0)
	    ret = ctxt->errNo;
	else
	    ret = -1;
    }
    ctxt->sax = oldsax;
    xmlFreeParserCtxt(ctxt);
    
    return ret;
}
#endif /* LIBXML_SAX1_ENABLED */

/**
 * xmlCreateDocParserCtxt:
 * @cur:  a pointer to an array of xmlChar
 *
 * Creates a parser context for an XML in-memory document.
 *
 * Returns the new parser context or NULL
 */
xmlParserCtxtPtr
xmlCreateDocParserCtxt(const xmlChar *cur) {
    int len;

    if (cur == NULL)
	return(NULL);
    len = xmlStrlen(cur);
    return(xmlCreateMemoryParserCtxt((const char *)cur, len));
}

#ifdef LIBXML_SAX1_ENABLED
/**
 * xmlSAXParseDoc:
 * @sax:  the SAX handler block
 * @cur:  a pointer to an array of xmlChar
 * @recovery:  work in recovery mode, i.e. tries to read no Well Formed
 *             documents
 *
 * parse an XML in-memory document and build a tree.
 * It use the given SAX function block to handle the parsing callback.
 * If sax is NULL, fallback to the default DOM tree building routines.
 * 
 * Returns the resulting document tree
 */

xmlDocPtr
xmlSAXParseDoc(xmlSAXHandlerPtr sax, xmlChar *cur, int recovery) {
    xmlDocPtr ret;
    xmlParserCtxtPtr ctxt;

    if (cur == NULL) return(NULL);


    ctxt = xmlCreateDocParserCtxt(cur);
    if (ctxt == NULL) return(NULL);
    if (sax != NULL) { 
        ctxt->sax = sax;
        ctxt->userData = NULL;
    }
    xmlDetectSAX2(ctxt);

    xmlParseDocument(ctxt);
    if ((ctxt->wellFormed) || recovery) ret = ctxt->myDoc;
    else {
       ret = NULL;
       xmlFreeDoc(ctxt->myDoc);
       ctxt->myDoc = NULL;
    }
    if (sax != NULL) 
	ctxt->sax = NULL;
    xmlFreeParserCtxt(ctxt);
    
    return(ret);
}

/**
 * xmlParseDoc:
 * @cur:  a pointer to an array of xmlChar
 *
 * parse an XML in-memory document and build a tree.
 * 
 * Returns the resulting document tree
 */

xmlDocPtr
xmlParseDoc(xmlChar *cur) {
    return(xmlSAXParseDoc(NULL, cur, 0));
}
#endif /* LIBXML_SAX1_ENABLED */

#ifdef LIBXML_LEGACY_ENABLED
/************************************************************************
 *									*
 * 	Specific function to keep track of entities references		*
 * 	and used by the XSLT debugger					*
 *									*
 ************************************************************************/

static xmlEntityReferenceFunc xmlEntityRefFunc = NULL;

/**
 * xmlAddEntityReference:
 * @ent : A valid entity
 * @firstNode : A valid first node for children of entity
 * @lastNode : A valid last node of children entity 
 *
 * Notify of a reference to an entity of type XML_EXTERNAL_GENERAL_PARSED_ENTITY
 */
static void
xmlAddEntityReference(xmlEntityPtr ent, xmlNodePtr firstNode,
                      xmlNodePtr lastNode)
{
    if (xmlEntityRefFunc != NULL) {
        (*xmlEntityRefFunc) (ent, firstNode, lastNode);
    }
}


/**
 * xmlSetEntityReferenceFunc:
 * @func: A valid function
 *
 * Set the function to call call back when a xml reference has been made
 */
void
xmlSetEntityReferenceFunc(xmlEntityReferenceFunc func)
{
    xmlEntityRefFunc = func;
}
#endif /* LIBXML_LEGACY_ENABLED */

/************************************************************************
 *									*
 * 				Miscellaneous				*
 *									*
 ************************************************************************/

#ifdef LIBXML_XPATH_ENABLED
#include <libxml/xpath.h>
#endif

extern void xmlGenericErrorDefaultFunc(void *ctx, const char *msg, ...);
static int xmlParserInitialized = 0;

/**
 * xmlInitParser:
 *
 * Initialization function for the XML parser.
 * This is not reentrant. Call once before processing in case of
 * use in multithreaded programs.
 */

void
xmlInitParser(void) {
    if (xmlParserInitialized != 0)
	return;

    if ((xmlGenericError == xmlGenericErrorDefaultFunc) ||
	(xmlGenericError == NULL))
	initGenericErrorDefaultFunc(NULL);
    xmlInitGlobals();
    xmlInitThreads();
    xmlInitMemory();
    xmlInitCharEncodingHandlers();
    xmlDefaultSAXHandlerInit();
    xmlRegisterDefaultInputCallbacks();
#ifdef LIBXML_OUTPUT_ENABLED
    xmlRegisterDefaultOutputCallbacks();
#endif /* LIBXML_OUTPUT_ENABLED */
#ifdef LIBXML_HTML_ENABLED
    htmlInitAutoClose();
    htmlDefaultSAXHandlerInit();
#endif
#ifdef LIBXML_XPATH_ENABLED
    xmlXPathInit();
#endif
    xmlParserInitialized = 1;
}

/**
 * xmlCleanupParser:
 *
 * Cleanup function for the XML parser. It tries to reclaim all
 * parsing related global memory allocated for the parser processing.
 * It doesn't deallocate any document related memory. Calling this
 * function should not prevent reusing the parser.
 * One should call xmlCleanupParser() only when the process has
 * finished using the library or XML document built with it.
 */

void
xmlCleanupParser(void) {
    if (!xmlParserInitialized)
	return;

    xmlCleanupCharEncodingHandlers();
#ifdef LIBXML_CATALOG_ENABLED
    xmlCatalogCleanup();
#endif
    xmlCleanupInputCallbacks();
#ifdef LIBXML_OUTPUT_ENABLED
    xmlCleanupOutputCallbacks();
#endif
    xmlCleanupThreads();
    xmlCleanupGlobals();
    xmlResetLastError();
    xmlParserInitialized = 0;
}

/************************************************************************
 *									*
 *	New set (2.6.0) of simpler and more flexible APIs		*
 *									*
 ************************************************************************/

/**
 * DICT_FREE:
 * @str:  a string
 *
 * Free a string if it is not owned by the "dict" dictionnary in the
 * current scope
 */
#define DICT_FREE(str)						\
	if ((str) && ((!dict) || 				\
	    (xmlDictOwns(dict, (const xmlChar *)(str)) == 0)))	\
	    xmlFree((char *)(str));

/**
 * xmlCtxtReset:
 * @ctxt: an XML parser context
 *
 * Reset a parser context
 */
void
xmlCtxtReset(xmlParserCtxtPtr ctxt)
{
    xmlParserInputPtr input;
    xmlDictPtr dict = ctxt->dict;

    while ((input = inputPop(ctxt)) != NULL) { /* Non consuming */
        xmlFreeInputStream(input);
    }
    ctxt->inputNr = 0;
    ctxt->input = NULL;

    ctxt->spaceNr = 0;
    ctxt->spaceTab[0] = -1;
    ctxt->space = &ctxt->spaceTab[0];


    ctxt->nodeNr = 0;
    ctxt->node = NULL;

    ctxt->nameNr = 0;
    ctxt->name = NULL;

    DICT_FREE(ctxt->version);
    ctxt->version = NULL;
    DICT_FREE(ctxt->encoding);
    ctxt->encoding = NULL;
    DICT_FREE(ctxt->directory);
    ctxt->directory = NULL;
    DICT_FREE(ctxt->extSubURI);
    ctxt->extSubURI = NULL;
    DICT_FREE(ctxt->extSubSystem);
    ctxt->extSubSystem = NULL;
    if (ctxt->myDoc != NULL)
        xmlFreeDoc(ctxt->myDoc);
    ctxt->myDoc = NULL;

    ctxt->standalone = -1;
    ctxt->hasExternalSubset = 0;
    ctxt->hasPErefs = 0;
    ctxt->html = 0;
    ctxt->external = 0;
    ctxt->instate = XML_PARSER_START;
    ctxt->token = 0;

    ctxt->wellFormed = 1;
    ctxt->nsWellFormed = 1;
    ctxt->valid = 1;
    ctxt->vctxt.userData = ctxt;
    ctxt->vctxt.error = xmlParserValidityError;
    ctxt->vctxt.warning = xmlParserValidityWarning;
    ctxt->record_info = 0;
    ctxt->nbChars = 0;
    ctxt->checkIndex = 0;
    ctxt->inSubset = 0;
    ctxt->errNo = XML_ERR_OK;
    ctxt->depth = 0;
    ctxt->charset = XML_CHAR_ENCODING_UTF8;
    ctxt->catalogs = NULL;
    xmlInitNodeInfoSeq(&ctxt->node_seq);

    if (ctxt->attsDefault != NULL) {
        xmlHashFree(ctxt->attsDefault, (xmlHashDeallocator) xmlFree);
        ctxt->attsDefault = NULL;
    }
    if (ctxt->attsSpecial != NULL) {
        xmlHashFree(ctxt->attsSpecial, NULL);
        ctxt->attsSpecial = NULL;
    }

#ifdef LIBXML_CATALOG_ENABLED
    if (ctxt->catalogs != NULL)
	xmlCatalogFreeLocal(ctxt->catalogs);
#endif
}

/**
 * xmlCtxtUseOptions:
 * @ctxt: an XML parser context
 * @options:  a combination of xmlParserOption(s)
 *
 * Applies the options to the parser context
 *
 * Returns 0 in case of success, the set of unknown or unimplemented options
 *         in case of error.
 */
int
xmlCtxtUseOptions(xmlParserCtxtPtr ctxt, int options)
{
    if (options & XML_PARSE_RECOVER) {
        ctxt->recovery = 1;
        options -= XML_PARSE_RECOVER;
    } else
        ctxt->recovery = 0;
    if (options & XML_PARSE_DTDLOAD) {
        ctxt->loadsubset = XML_DETECT_IDS;
        options -= XML_PARSE_DTDLOAD;
    } else
        ctxt->loadsubset = 0;
    if (options & XML_PARSE_DTDATTR) {
        ctxt->loadsubset |= XML_COMPLETE_ATTRS;
        options -= XML_PARSE_DTDATTR;
    }
    if (options & XML_PARSE_NOENT) {
        ctxt->replaceEntities = 1;
        /* ctxt->loadsubset |= XML_DETECT_IDS; */
        options -= XML_PARSE_NOENT;
    } else
        ctxt->replaceEntities = 0;
    if (options & XML_PARSE_NOWARNING) {
        ctxt->sax->warning = NULL;
        options -= XML_PARSE_NOWARNING;
    }
    if (options & XML_PARSE_NOERROR) {
        ctxt->sax->error = NULL;
        ctxt->sax->fatalError = NULL;
        options -= XML_PARSE_NOERROR;
    }
    if (options & XML_PARSE_PEDANTIC) {
        ctxt->pedantic = 1;
        options -= XML_PARSE_PEDANTIC;
    } else
        ctxt->pedantic = 0;
    if (options & XML_PARSE_NOBLANKS) {
        ctxt->keepBlanks = 0;
        ctxt->sax->ignorableWhitespace = xmlSAX2IgnorableWhitespace;
        options -= XML_PARSE_NOBLANKS;
    } else
        ctxt->keepBlanks = 1;
    if (options & XML_PARSE_DTDVALID) {
        ctxt->validate = 1;
        if (options & XML_PARSE_NOWARNING)
            ctxt->vctxt.warning = NULL;
        if (options & XML_PARSE_NOERROR)
            ctxt->vctxt.error = NULL;
        options -= XML_PARSE_DTDVALID;
    } else
        ctxt->validate = 0;
#ifdef LIBXML_SAX1_ENABLED
    if (options & XML_PARSE_SAX1) {
        ctxt->sax->startElement = xmlSAX2StartElement;
        ctxt->sax->endElement = xmlSAX2EndElement;
        ctxt->sax->startElementNs = NULL;
        ctxt->sax->endElementNs = NULL;
        ctxt->sax->initialized = 1;
        options -= XML_PARSE_SAX1;
    }
#endif /* LIBXML_SAX1_ENABLED */
    if (options & XML_PARSE_NODICT) {
        ctxt->dictNames = 0;
        options -= XML_PARSE_NODICT;
    } else {
        ctxt->dictNames = 1;
    }
    if (options & XML_PARSE_NOCDATA) {
        ctxt->sax->cdataBlock = NULL;
        options -= XML_PARSE_NOCDATA;
    }
    if (options & XML_PARSE_NSCLEAN) {
	ctxt->options |= XML_PARSE_NSCLEAN;
        options -= XML_PARSE_NSCLEAN;
    }
    return (options);
}

/**
 * xmlDoRead:
 * @ctxt:  an XML parser context
 * @URL:  the base URL to use for the document
 * @encoding:  the document encoding, or NULL
 * @options:  a combination of xmlParserOption(s)
 * @reuse:  keep the context for reuse
 *
 * Common front-end for the xmlRead functions
 * 
 * Returns the resulting document tree or NULL
 */
static xmlDocPtr
xmlDoRead(xmlParserCtxtPtr ctxt, const char *URL, const char *encoding,
          int options, int reuse)
{
    xmlDocPtr ret;
    
    xmlCtxtUseOptions(ctxt, options);
    if (encoding != NULL) {
        xmlCharEncodingHandlerPtr hdlr;

	hdlr = xmlFindCharEncodingHandler(encoding);
	if (hdlr != NULL)
	    xmlSwitchToEncoding(ctxt, hdlr);
    }
    if ((URL != NULL) && (ctxt->input != NULL) &&
        (ctxt->input->filename == NULL))
        ctxt->input->filename = (char *) xmlStrdup((const xmlChar *) URL);
    xmlParseDocument(ctxt);
    if ((ctxt->wellFormed) || ctxt->recovery)
        ret = ctxt->myDoc;
    else {
        ret = NULL;
	if (ctxt->myDoc != NULL) {
	    if ((ctxt->dictNames) &&
		(ctxt->myDoc->dict == ctxt->dict))
		xmlDictReference(ctxt->dict);
	    xmlFreeDoc(ctxt->myDoc);
	}
    }
    ctxt->myDoc = NULL;
    if (!reuse) {
        if ((ctxt->dictNames) &&
	    (ret != NULL) &&
	    (ret->dict == ctxt->dict))
	    ctxt->dict = NULL;
	xmlFreeParserCtxt(ctxt);
    } else {
        /* Must duplicate the reference to the dictionary */
        if ((ctxt->dictNames) &&
	    (ret != NULL) &&
	    (ret->dict == ctxt->dict))
	    xmlDictReference(ctxt->dict);
    }

    return (ret);
}

/**
 * xmlReadDoc:
 * @cur:  a pointer to a zero terminated string
 * @URL:  the base URL to use for the document
 * @encoding:  the document encoding, or NULL
 * @options:  a combination of xmlParserOption(s)
 *
 * parse an XML in-memory document and build a tree.
 * 
 * Returns the resulting document tree
 */
xmlDocPtr
xmlReadDoc(const xmlChar * cur, const char *URL, const char *encoding, int options)
{
    xmlParserCtxtPtr ctxt;

    if (cur == NULL)
        return (NULL);

    ctxt = xmlCreateDocParserCtxt(cur);
    if (ctxt == NULL)
        return (NULL);
    return (xmlDoRead(ctxt, URL, encoding, options, 0));
}

/**
 * xmlReadFile:
 * @filename:  a file or URL
 * @encoding:  the document encoding, or NULL
 * @options:  a combination of xmlParserOption(s)
 *
 * parse an XML file from the filesystem or the network.
 * 
 * Returns the resulting document tree
 */
xmlDocPtr
xmlReadFile(const char *filename, const char *encoding, int options)
{
    xmlParserCtxtPtr ctxt;

    ctxt = xmlCreateFileParserCtxt(filename);
    if (ctxt == NULL)
        return (NULL);
    return (xmlDoRead(ctxt, NULL, encoding, options, 0));
}

/**
 * xmlReadMemory:
 * @buffer:  a pointer to a char array
 * @size:  the size of the array
 * @URL:  the base URL to use for the document
 * @encoding:  the document encoding, or NULL
 * @options:  a combination of xmlParserOption(s)
 *
 * parse an XML in-memory document and build a tree.
 * 
 * Returns the resulting document tree
 */
xmlDocPtr
xmlReadMemory(const char *buffer, int size, const char *URL, const char *encoding, int options)
{
    xmlParserCtxtPtr ctxt;

    ctxt = xmlCreateMemoryParserCtxt(buffer, size);
    if (ctxt == NULL)
        return (NULL);
    return (xmlDoRead(ctxt, URL, encoding, options, 0));
}

/**
 * xmlReadFd:
 * @fd:  an open file descriptor
 * @URL:  the base URL to use for the document
 * @encoding:  the document encoding, or NULL
 * @options:  a combination of xmlParserOption(s)
 *
 * parse an XML from a file descriptor and build a tree.
 * 
 * Returns the resulting document tree
 */
xmlDocPtr
xmlReadFd(int fd, const char *URL, const char *encoding, int options)
{
    xmlParserCtxtPtr ctxt;
    xmlParserInputBufferPtr input;
    xmlParserInputPtr stream;

    if (fd < 0)
        return (NULL);

    input = xmlParserInputBufferCreateFd(fd, XML_CHAR_ENCODING_NONE);
    if (input == NULL)
        return (NULL);
    ctxt = xmlNewParserCtxt();
    if (ctxt == NULL) {
        xmlFreeParserInputBuffer(input);
        return (NULL);
    }
    stream = xmlNewIOInputStream(ctxt, input, XML_CHAR_ENCODING_NONE);
    if (stream == NULL) {
        xmlFreeParserInputBuffer(input);
	xmlFreeParserCtxt(ctxt);
        return (NULL);
    }
    inputPush(ctxt, stream);
    return (xmlDoRead(ctxt, URL, encoding, options, 0));
}

/**
 * xmlReadIO:
 * @ioread:  an I/O read function
 * @ioclose:  an I/O close function
 * @ioctx:  an I/O handler
 * @URL:  the base URL to use for the document
 * @encoding:  the document encoding, or NULL
 * @options:  a combination of xmlParserOption(s)
 *
 * parse an XML document from I/O functions and source and build a tree.
 * 
 * Returns the resulting document tree
 */
xmlDocPtr
xmlReadIO(xmlInputReadCallback ioread, xmlInputCloseCallback ioclose,
          void *ioctx, const char *URL, const char *encoding, int options)
{
    xmlParserCtxtPtr ctxt;
    xmlParserInputBufferPtr input;
    xmlParserInputPtr stream;

    if (ioread == NULL)
        return (NULL);

    input = xmlParserInputBufferCreateIO(ioread, ioclose, ioctx,
                                         XML_CHAR_ENCODING_NONE);
    if (input == NULL)
        return (NULL);
    ctxt = xmlNewParserCtxt();
    if (ctxt == NULL) {
        xmlFreeParserInputBuffer(input);
        return (NULL);
    }
    stream = xmlNewIOInputStream(ctxt, input, XML_CHAR_ENCODING_NONE);
    if (stream == NULL) {
        xmlFreeParserInputBuffer(input);
	xmlFreeParserCtxt(ctxt);
        return (NULL);
    }
    inputPush(ctxt, stream);
    return (xmlDoRead(ctxt, URL, encoding, options, 0));
}

/**
 * xmlCtxtReadDoc:
 * @ctxt:  an XML parser context
 * @cur:  a pointer to a zero terminated string
 * @URL:  the base URL to use for the document
 * @encoding:  the document encoding, or NULL
 * @options:  a combination of xmlParserOption(s)
 *
 * parse an XML in-memory document and build a tree.
 * This reuses the existing @ctxt parser context
 * 
 * Returns the resulting document tree
 */
xmlDocPtr
xmlCtxtReadDoc(xmlParserCtxtPtr ctxt, const xmlChar * cur,
               const char *URL, const char *encoding, int options)
{
    xmlParserInputPtr stream;

    if (cur == NULL)
        return (NULL);
    if (ctxt == NULL)
        return (NULL);

    xmlCtxtReset(ctxt);

    stream = xmlNewStringInputStream(ctxt, cur);
    if (stream == NULL) {
        return (NULL);
    }
    inputPush(ctxt, stream);
    return (xmlDoRead(ctxt, URL, encoding, options, 1));
}

/**
 * xmlCtxtReadFile:
 * @ctxt:  an XML parser context
 * @filename:  a file or URL
 * @encoding:  the document encoding, or NULL
 * @options:  a combination of xmlParserOption(s)
 *
 * parse an XML file from the filesystem or the network.
 * This reuses the existing @ctxt parser context
 * 
 * Returns the resulting document tree
 */
xmlDocPtr
xmlCtxtReadFile(xmlParserCtxtPtr ctxt, const char *filename,
                const char *encoding, int options)
{
    xmlParserInputPtr stream;

    if (filename == NULL)
        return (NULL);
    if (ctxt == NULL)
        return (NULL);

    xmlCtxtReset(ctxt);

    stream = xmlNewInputFromFile(ctxt, filename);
    if (stream == NULL) {
        return (NULL);
    }
    inputPush(ctxt, stream);
    return (xmlDoRead(ctxt, NULL, encoding, options, 1));
}

/**
 * xmlCtxtReadMemory:
 * @ctxt:  an XML parser context
 * @buffer:  a pointer to a char array
 * @size:  the size of the array
 * @URL:  the base URL to use for the document
 * @encoding:  the document encoding, or NULL
 * @options:  a combination of xmlParserOption(s)
 *
 * parse an XML in-memory document and build a tree.
 * This reuses the existing @ctxt parser context
 * 
 * Returns the resulting document tree
 */
xmlDocPtr
xmlCtxtReadMemory(xmlParserCtxtPtr ctxt, const char *buffer, int size,
                  const char *URL, const char *encoding, int options)
{
    xmlParserInputBufferPtr input;
    xmlParserInputPtr stream;

    if (ctxt == NULL)
        return (NULL);
    if (buffer == NULL)
        return (NULL);

    xmlCtxtReset(ctxt);

    input = xmlParserInputBufferCreateMem(buffer, size, XML_CHAR_ENCODING_NONE);
    if (input == NULL) {
	return(NULL);
    }

    stream = xmlNewIOInputStream(ctxt, input, XML_CHAR_ENCODING_NONE);
    if (stream == NULL) {
	xmlFreeParserInputBuffer(input);
	return(NULL);
    }

    inputPush(ctxt, stream);
    return (xmlDoRead(ctxt, URL, encoding, options, 1));
}

/**
 * xmlCtxtReadFd:
 * @ctxt:  an XML parser context
 * @fd:  an open file descriptor
 * @URL:  the base URL to use for the document
 * @encoding:  the document encoding, or NULL
 * @options:  a combination of xmlParserOption(s)
 *
 * parse an XML from a file descriptor and build a tree.
 * This reuses the existing @ctxt parser context
 * 
 * Returns the resulting document tree
 */
xmlDocPtr
xmlCtxtReadFd(xmlParserCtxtPtr ctxt, int fd,
              const char *URL, const char *encoding, int options)
{
    xmlParserInputBufferPtr input;
    xmlParserInputPtr stream;

    if (fd < 0)
        return (NULL);
    if (ctxt == NULL)
        return (NULL);

    xmlCtxtReset(ctxt);


    input = xmlParserInputBufferCreateFd(fd, XML_CHAR_ENCODING_NONE);
    if (input == NULL)
        return (NULL);
    stream = xmlNewIOInputStream(ctxt, input, XML_CHAR_ENCODING_NONE);
    if (stream == NULL) {
        xmlFreeParserInputBuffer(input);
        return (NULL);
    }
    inputPush(ctxt, stream);
    return (xmlDoRead(ctxt, URL, encoding, options, 1));
}

/**
 * xmlCtxtReadIO:
 * @ctxt:  an XML parser context
 * @ioread:  an I/O read function
 * @ioclose:  an I/O close function
 * @ioctx:  an I/O handler
 * @URL:  the base URL to use for the document
 * @encoding:  the document encoding, or NULL
 * @options:  a combination of xmlParserOption(s)
 *
 * parse an XML document from I/O functions and source and build a tree.
 * This reuses the existing @ctxt parser context
 * 
 * Returns the resulting document tree
 */
xmlDocPtr
xmlCtxtReadIO(xmlParserCtxtPtr ctxt, xmlInputReadCallback ioread,
              xmlInputCloseCallback ioclose, void *ioctx,
	      const char *URL,
              const char *encoding, int options)
{
    xmlParserInputBufferPtr input;
    xmlParserInputPtr stream;

    if (ioread == NULL)
        return (NULL);
    if (ctxt == NULL)
        return (NULL);

    xmlCtxtReset(ctxt);

    input = xmlParserInputBufferCreateIO(ioread, ioclose, ioctx,
                                         XML_CHAR_ENCODING_NONE);
    if (input == NULL)
        return (NULL);
    stream = xmlNewIOInputStream(ctxt, input, XML_CHAR_ENCODING_NONE);
    if (stream == NULL) {
        xmlFreeParserInputBuffer(input);
        return (NULL);
    }
    inputPush(ctxt, stream);
    return (xmlDoRead(ctxt, URL, encoding, options, 1));
}
