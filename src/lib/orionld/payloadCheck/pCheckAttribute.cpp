/*
*
* Copyright 2022 FIWARE Foundation e.V.
*
* This file is part of Orion-LD Context Broker.
*
* Orion-LD Context Broker is free software: you can redistribute it and/or
* modify it under the terms of the GNU Affero General Public License as
* published by the Free Software Foundation, either version 3 of the
* License, or (at your option) any later version.
*
* Orion-LD Context Broker is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero
* General Public License for more details.
*
* You should have received a copy of the GNU Affero General Public License
* along with Orion-LD Context Broker. If not, see http://www.gnu.org/licenses/.
*
* For those usages not covered by this license please contact with
* orionld at fiware dot org
*
* Author: Ken Zangelin
*/
#include <string.h>                                              // strcmp
#include <unistd.h>                                              // NULL

extern "C"
{
#include "kalloc/kaStrdup.h"                                     // kaStrdup
#include "kjson/KjNode.h"                                        // KjNode
#include "kjson/kjLookup.h"                                      // kjLookup
#include "kjson/kjBuilder.h"                                     // kjString, kjObject, ...
}

#include "logMsg/logMsg.h"                                       // LM_*

#include "orionld/types/OrionldProblemDetails.h"                 // OrionldProblemDetails
#include "orionld/types/OrionldResponseErrorType.h"              // OrionldBadRequestData
#include "orionld/types/OrionldAttributeType.h"                  // OrionldAttributeType, orionldAttributeTypeName
#include "orionld/common/orionldState.h"                         // orionldState
#include "orionld/common/orionldError.h"                         // orionldError
#include "orionld/context/OrionldContextItem.h"                  // OrionldContextItem
#include "orionld/context/orionldAttributeExpand.h"              // orionldAttributeExpand
#include "orionld/context/orionldSubAttributeExpand.h"           // orionldSubAttributeExpand
#include "orionld/rest/OrionLdRestService.h"                     // ORIONLD_SERVICE_OPTION_ACCEPT_JSONLD_NULL
#include "orionld/kjTree/kjJsonldNullObject.h"                   // kjJsonldNullObject
#include "orionld/serviceRoutines/orionldPatchEntity2.h"         // orionldPatchEntity2
#include "orionld/payloadCheck/PCHECK.h"                         // PCHECK_*
#include "orionld/payloadCheck/pcheckName.h"                     // pCheckName
#include "orionld/payloadCheck/pCheckUri.h"                      // pCheckUri
#include "orionld/payloadCheck/pCheckAttributeTransform.h"       // pCheckAttributeTransform
#include "orionld/payloadCheck/pCheckGeoPropertyValue.h"         // pCheckGeoPropertyValue
#include "orionld/payloadCheck/pCheckAttributeType.h"            // pCheckAttributeType
#include "orionld/payloadCheck/pCheckAttribute.h"                // Own interface



// -----------------------------------------------------------------------------
//
// attrTypeChangeTitle -
//
static const char* attrTypeChangeTitle(OrionldAttributeType oldType, OrionldAttributeType newType)
{
  if (newType == Property)
  {
    if (oldType == Relationship)      return "Attempt to transform a Relationship into a Property";
    if (oldType == GeoProperty)       return "Attempt to transform a GeoProperty into a Property";
    if (oldType == LanguageProperty)  return "Attempt to transform a LanguageProperty into a Property";
  }
  else if (newType == Relationship)
  {
    if (oldType == Property)          return "Attempt to transform a Property into a Relationship";
    if (oldType == GeoProperty)       return "Attempt to transform a GeoProperty into a Relationship";
    if (oldType == LanguageProperty)  return "Attempt to transform a LanguageProperty into a Relationship";
  }
  else if (newType == GeoProperty)
  {
    if (oldType == Property)          return "Attempt to transform a Property into a GeoProperty";
    if (oldType == Relationship)      return "Attempt to transform a Relationship into a GeoProperty";
    if (oldType == LanguageProperty)  return "Attempt to transform a LanguageProperty into a GeoProperty";
  }
  else if (newType == LanguageProperty)
  {
    if (oldType == Property)          return "Attempt to transform a Property into a LanguageProperty";
    if (oldType == Relationship)      return "Attempt to transform a Relationship into a LanguageProperty";
    if (oldType == GeoProperty)       return "Attempt to transform a GeoProperty into a LanguageProperty";
  }

  return "Attribute type inconsistency";
}



// -----------------------------------------------------------------------------
//
// pCheckTypeFromContext -
//
bool pCheckTypeFromContext(KjNode* attrP, OrionldContextItem* attrContextInfoP)
{
  bool arrayReduction = true;

  if ((attrContextInfoP != NULL) && (attrContextInfoP->type != NULL))
  {
    if (strcmp(attrContextInfoP->type, "DateTime") == 0)
    {
      if (attrP->type != KjString)
      {
        orionldError(OrionldBadRequestData,
                     "JSON Type for attribute not according to @context @type field",
                     attrP->name,
                     400);
        return false;
      }

      if (parse8601Time(attrP->value.s) == -1)
      {
        orionldError(OrionldBadRequestData,
                     "Not a valid ISO8601 DateTime",
                     attrP->name,
                     400);
        return false;
      }
    }
    else if (strcmp(attrContextInfoP->type, "@id") == 0)
    {
      if (attrP->type != KjString)
      {
        orionldError(OrionldBadRequestData,
                     "JSON Type for attribute not according to @context @type field",
                     attrP->name,
                     400);
        return false;
      }

      if (pCheckUri(attrP->value.s, attrP->name, true) == false)
      {
        orionldError(OrionldBadRequestData,
                     "Not a valid URI",
                     attrP->name,
                     400);
        return false;
      }
    }
    else if (strcmp(attrContextInfoP->type, "@set") == 0)
      arrayReduction = false;
    else if (strcmp(attrContextInfoP->type, "@list") == 0)
      arrayReduction = false;
  }

  //
  // Array Reduction
  //
  if ((attrP->type == KjArray) && (arrayReduction == true))
  {
    if ((attrP->value.firstChildP != NULL) && (attrP->value.firstChildP->next == NULL))
    {
      KjNode* arrayItemP = attrP->value.firstChildP;

      attrP->type      = arrayItemP->type;
      attrP->value     = arrayItemP->value;
      attrP->lastChild = arrayItemP->lastChild;  // Might be an array or object inside the array ...
    }
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// pCheckAttributeString -
//
// A String is always a Property.
// Make sure that:
// - if the attribute already existed, that it is a Property in the DB
// - OR: it's a new attribute
//
inline bool pCheckAttributeString
(
  KjNode*               attrP,
  bool                  isAttribute,
  OrionldAttributeType  attrTypeFromDb,
  OrionldContextItem*   attrContextInfoP
)
{
  PCHECK_SPECIAL_ATTRIBUTE(attrP, isAttribute, attrTypeFromDb);

  // Special case:
  //   options=keyValues is set
  //   The service routine is orionldPatchEntity2
  //   The attribute already exists in the DB
  //   The attribute is a Relationship in the DB
  //   The new value is a JSON String, that is a valid URI
  //
  // If all those are fulfilled, then the value (object) of the Relationship will be modified
  //
  if ((orionldState.uriParamOptions.keyValues == true)                &&
      (orionldState.serviceP->serviceRoutine  == orionldPatchEntity2) &&
      (attrTypeFromDb                         == Relationship))
  {
    if (pCheckUri(attrP->value.s, attrP->name, true) == false)
    {
      orionldError(OrionldBadRequestData, "PATCH of Relationship - not a valid URI", attrP->name, 400);
      return false;
    }

    KjNode* valueP = kjString(orionldState.kjsonP, "value", attrP->value.s);  // "value" or "object" ... ?
    pCheckAttributeTransform(attrP, "Relationship", valueP);
    return true;
  }

  PCHECK_NOT_A_PROPERTY(attrP, attrTypeFromDb);

  KjNode* valueP = kjString(orionldState.kjsonP, "value", attrP->value.s);
  pCheckAttributeTransform(attrP, "Property", valueP);
  return true;
}



// -----------------------------------------------------------------------------
//
// pCheckAttributeInteger -
//
inline bool pCheckAttributeInteger
(
  KjNode*               attrP,
  bool                  isAttribute,
  OrionldAttributeType  attrTypeFromDb,
  OrionldContextItem*   attrContextInfoP
)
{
  PCHECK_SPECIAL_ATTRIBUTE(attrP, isAttribute, attrTypeFromDb);
  PCHECK_NOT_A_PROPERTY(attrP, attrTypeFromDb);

  KjNode* valueP = kjInteger(orionldState.kjsonP, "value", attrP->value.i);
  pCheckAttributeTransform(attrP, "Property", valueP);
  return true;
}



// -----------------------------------------------------------------------------
//
// pCheckAttributeFloat -
//
inline bool pCheckAttributeFloat
(
  KjNode*               attrP,
  bool                  isAttribute,
  OrionldAttributeType  attrTypeFromDb,
  OrionldContextItem*   attrContextInfoP
)
{
  PCHECK_SPECIAL_ATTRIBUTE(attrP, isAttribute, attrTypeFromDb);
  PCHECK_NOT_A_PROPERTY(attrP, attrTypeFromDb);

  KjNode* valueP = kjFloat(orionldState.kjsonP,  "value", attrP->value.f);
  pCheckAttributeTransform(attrP, "Property", valueP);
  return true;
}



// -----------------------------------------------------------------------------
//
// pCheckAttributeBoolean -
//
inline bool pCheckAttributeBoolean
(
  KjNode*               attrP,
  bool                  isAttribute,
  OrionldAttributeType  attrTypeFromDb,
  OrionldContextItem*   attrContextInfoP
)
{
  PCHECK_SPECIAL_ATTRIBUTE(attrP, isAttribute, attrTypeFromDb);
  PCHECK_NOT_A_PROPERTY(attrP, attrTypeFromDb);

  KjNode* valueP = kjBoolean(orionldState.kjsonP,  "value", attrP->value.b);
  pCheckAttributeTransform(attrP, "Property", valueP);
  return true;
}



// -----------------------------------------------------------------------------
//
// pCheckAttributeArray -
//
inline bool pCheckAttributeArray
(
  KjNode*               attrP,
  bool                  isAttribute,
  OrionldAttributeType  attrTypeFromDb,
  OrionldContextItem*   attrContextInfoP
)
{
  PCHECK_SPECIAL_ATTRIBUTE(attrP, isAttribute, attrTypeFromDb);
  PCHECK_NOT_A_PROPERTY(attrP, attrTypeFromDb);

  KjNode* valueP = kjArray(orionldState.kjsonP,  "value");

  valueP->value.firstChildP = attrP->value.firstChildP;
  valueP->lastChild         = attrP->lastChild;

  pCheckAttributeTransform(attrP, "Property", valueP);

  return true;
}



// -----------------------------------------------------------------------------
//
// pCheckAttributeNull -
//
inline bool pCheckAttributeNull(KjNode* attrP)
{
#if 0
  LM_W(("RHS for attribute '%s' is NULL - that is forbidden in the NGSI-LD API", attrP->name));
  orionldError(OrionldBadRequestData,
               "The use of NULL value is banned in NGSI-LD",
               attrP->name,
               400);
  return false;
#else
  return true;
#endif
}



// -----------------------------------------------------------------------------
//
// valueAndTypeCheck -
//
bool valueAndTypeCheck(KjNode* attrP, OrionldAttributeType attributeType, bool attributeExisted)
{
  KjNode* valueP       = kjLookup(attrP, "value");
  KjNode* objectP      = kjLookup(attrP, "object");
  KjNode* languageMapP = kjLookup(attrP, "languageMap");

  if (attributeType == Property)
  {
    if (objectP != NULL)
    {
      orionldError(OrionldBadRequestData, "Forbidden field for a Property: object", attrP->name, 400);
      return false;
    }
    else if (languageMapP != NULL)
    {
      orionldError(OrionldBadRequestData, "Forbidden field for a Property: languageMap", attrP->name, 400);
      return false;
    }
    else if ((valueP == NULL) && (attributeExisted == false))  // Attribute is new but the value is missing
    {
      orionldError(OrionldBadRequestData, "Missing /value/ field for Property at creation time", attrP->name, 400);
      return false;
    }
  }
  if (attributeType == GeoProperty)
  {
    if (objectP != NULL)
    {
      orionldError(OrionldBadRequestData, "Forbidden field for a GeoProperty: object", attrP->name, 400);
      return false;
    }
    else if (languageMapP != NULL)
    {
      orionldError(OrionldBadRequestData, "Forbidden field for a GeoProperty: languageMap", attrP->name, 400);
      return false;
    }
    else if ((valueP == NULL) && (attributeExisted == false))  // Attribute is new but the value is missing
    {
      orionldError(OrionldBadRequestData, "Missing /value/ field for GeoProperty at creation time", attrP->name, 400);
      return false;
    }
  }
  else if (attributeType == Relationship)
  {
    if (valueP != NULL)
    {
      orionldError(OrionldBadRequestData, "Forbidden field for a Relationship: value", attrP->name, 400);
      return false;
    }
    else if (languageMapP != NULL)
    {
      orionldError(OrionldBadRequestData, "Forbidden field for a Relationship: languageMap", attrP->name, 400);
      return false;
    }
    else if ((objectP == NULL) && (attributeExisted == false))  // Attribute is new but the value is missing
    {
      orionldError(OrionldBadRequestData, "Missing /object/ field for Relationship at creation time", attrP->name, 400);
      return false;
    }
  }
  else if (attributeType == LanguageProperty)
  {
    if (valueP != NULL)
    {
      orionldError(OrionldBadRequestData, "Forbidden field for a LanguageProperty: value", attrP->name, 400);
      return false;
    }
    else if (objectP != NULL)
    {
      orionldError(OrionldBadRequestData, "Forbidden field for a LanguageProperty: object", attrP->name, 400);
      return false;
    }
    else if ((languageMapP == NULL) && (attributeExisted == false))  // Attribute is new but the value is missing
    {
      orionldError(OrionldBadRequestData, "Missing /languageMap/ field for LanguageProperty at creation time", attrP->name, 400);
      return false;
    }
  }

  return true;
}


// -----------------------------------------------------------------------------
//
// FIXME - IMPLEMENT
//
bool languageMapCheck(KjNode* fieldP) { return true; }



// -----------------------------------------------------------------------------
//
// datasetIdCheck -
//
bool datasetIdCheck(KjNode* fieldP)
{
  if (fieldP->type != KjString)
  {
    orionldError(OrionldBadRequestData, "Invalid JSON  type - not a string", fieldP->name, 400);
    return false;
  }

  if (pCheckUri(fieldP->value.s, fieldP->name, true) == false)
    return false;

  return true;
}


// -----------------------------------------------------------------------------
//
// objectCheck -
//
bool objectCheck(KjNode* fieldP)
{
  if (fieldP->type != KjString)
  {
    orionldError(OrionldBadRequestData, "Invalid JSON  type - not a string", fieldP->name, 400);
    return false;
  }

  if (pCheckUri(fieldP->value.s, fieldP->name, true) == false)
    return false;

  return true;
}



// -----------------------------------------------------------------------------
//
// stringArrayCheck -
//
// NOTE
//   A Relationship must have an "object field that is a string that is a valid URI.
//   However, Orion-LD (against the ETSI NGSI-LD API spec) allows for the "object" field to also ne an array of URIs.
//   Once datasetId is fully implemented, this will no longer be allowed.
//
bool stringArrayCheck(KjNode* arrayP)
{
  if (arrayP->type != KjArray)
    return false;

  for (KjNode* uriP = arrayP->value.firstChildP; uriP != NULL; uriP = uriP->next)
  {
    if (objectCheck(uriP) == false)
      return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// unitCodeCheck -
//
bool unitCodeCheck(KjNode* fieldP)
{
  if (fieldP->type != KjString)
  {
    orionldError(OrionldBadRequestData, "Invalid JSON  type - not a string", fieldP->name, 400);
    return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// timestampCheck -
//
bool timestampCheck(KjNode* fieldP)
{
  if (fieldP->type != KjString)
  {
    orionldError(OrionldBadRequestData, "Invalid JSON  type - not a string (so, not a valid timestamp)", fieldP->name, 400);
    return false;
  }

  if (parse8601Time(fieldP->value.s) == -1)
  {
    orionldError(OrionldBadRequestData, "Not a valid ISO8601 DateTime", fieldP->name, 400);
    return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// isGeoJsonValue -
//
static bool isGeoJsonValue(KjNode* valueP)
{
  if (valueP->type != KjObject)
    return false;

  bool coordinatesPresent  = false;
  bool typePresent         = false;
  int  nodes               = 0;

  for (KjNode* nodeP = valueP->value.firstChildP; nodeP != NULL; nodeP = nodeP->next)
  {
    ++nodes;

    if (nodes >= 3)
      return false;

    if (strcmp(nodeP->name, "type") == 0)
    {
      if (orionldGeoJsonTypeFromString(nodeP->value.s) != GeoJsonNoType)
        typePresent = true;
    }
    else if (strcmp(nodeP->name, "coordinates") == 0)
    {
      if (nodeP->type == KjArray)
        coordinatesPresent = true;
    }
  }

  if ((typePresent == true) && (coordinatesPresent == true))
    return true;

  return false;
}


// -----------------------------------------------------------------------------
//
// multiAttributeArray -
//
bool multiAttributeArray(KjNode* attrArrayP, bool* errorP)
{
  int datasets = 0;
  int objects  = 0;

  for (KjNode* aInstanceP = attrArrayP->value.firstChildP; aInstanceP != NULL; aInstanceP = aInstanceP->next)
  {
    if (aInstanceP->type != KjObject)
      return false;

    KjNode* datasetIdP = kjLookup(aInstanceP, "datasetId");

    if (datasetIdP != NULL)
      ++datasets;

    ++objects;
  }

  if (datasets == 0)
    return false;

  if (objects - datasets > 1)  // More than one object without datasetId field
  {
      orionldError(OrionldBadRequestData, "More than one default attribute in dataset array", attrArrayP->name, 400);
      *errorP = true;
      return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// pCheckAttributeObject -
//
// - if "type" is present inside the object:
//   - and it's a valid NGSI-LD Attribute type name ("Property", "Relationship, "GeoProperty", ...)
//     then the type of the attribute is DECIDED
//   - else ("type" is not a valid type) AND "coordinates" present, and nothing else,
//     it's considered a GeoProperty - procedure as for SIMPLE formats
// - if "type" is NOT present:
//   - if "value/object/languageMap" is present, then we can deduct the attribute type and add it to the object. Sub-attrs are processed
//   - if no value is present, then no type is added, only sub-attrs are processed.
//
static bool pCheckAttributeObject
(
  KjNode*               attrP,
  bool                  isAttribute,
  OrionldAttributeType  attrTypeFromDb,
  OrionldContextItem*   attrContextInfoP
)
{
  OrionldAttributeType  attributeType = NoAttributeType;
  KjNode*               typeP;

  // Check for errors in the input payload for the attribute type
  if (pCheckAttributeType(attrP, &typeP, false) == false)
    return false;

  //
  // If the attribute already exists (in the database), we KNOW the type of the attribute.
  // If the payload body contains a type, and it's not the same type, well, that's an error right there
  //
  // However, the payload body *might* be the value of a GeoProperty.
  // If so, the payload body would have a type == "Point", "Polygon" or something similar.
  // We CAN'T compare the type with what "should be according to the DB" until we rule GeoProperty value out.
  //
  // That is done a little later in this function - see "All good, let's now compare with the truth"
  //
  if (typeP != NULL)  // Attribute Type given in incoming payload
  {
    //
    // Check for the special JSON-LD null object - only valid for PATCH /entities/{eId} ...
    //
    if ((orionldState.serviceP->options & ORIONLD_SERVICE_OPTION_ACCEPT_JSONLD_NULL) != 0)
    {
      if (kjJsonldNullObject(attrP, typeP) == true)
        return true;
    }

    //
    // If type is in the payload:
    //   Is it valid?
    //   - Either "Property", "Relationhip", etc
    //   - OR, GeoJSON type (Point, Polygon) AND coordinates member and only those two   => GeoProperty
    //   - IF NOT valid, then "Attribute Type Error"
    //   - IF VALID, if also 'attrTypeFromDb != NoAttributeType', compare => "attempt to change attr type"
    //
    bool geoJsonValue = false;

    attributeType = orionldAttributeType(typeP->value.s);
    if (attributeType == NoAttributeType)
    {
      if (isGeoJsonValue(attrP))
      {
        attributeType = GeoProperty;
        geoJsonValue  = true;
      }
      else
      {
        orionldError(OrionldBadRequestData, "Invalid value for /type/", typeP->value.s, 400);
        return false;
      }
    }

    if ((attrTypeFromDb != NoAttributeType) && (attributeType != attrTypeFromDb))
    {
      const char* title = attrTypeChangeTitle(attrTypeFromDb, attributeType);
      orionldError(OrionldBadRequestData, title, attrP->name, 400);
      return false;
    }

    if (geoJsonValue == true)
    {
      if (pCheckGeoPropertyValue(attrP, typeP) == false)
      {
        LM_W(("pCheckGeoPropertyValue flagged an error: %s: %s", orionldState.pd.title, orionldState.pd.detail));
        return false;
      }

      return true;
    }

    // As "type" is present - is it coherent? (Property has "value", Relationship has "object", etc)
    if (valueAndTypeCheck(attrP, attributeType, attrTypeFromDb != NoAttributeType) == false)
      LM_RE(false, ("valueAndTypeCheck failed"));
  }
  else  // Attribute Type is not there - try to guess the type, if not already known from the DB
  {
    KjNode* valueP       = kjLookup(attrP, "value");
    KjNode* objectP      = kjLookup(attrP, "object");
    KjNode* languageMapP = kjLookup(attrP, "languageMap");

    if (valueP != NULL)
    {
      if (isGeoJsonValue(valueP) == true)
        attributeType = GeoProperty;
      else
        attributeType = Property;
    }
    else if (objectP != NULL)
      attributeType = Relationship;
    else if (languageMapP != NULL)
      attributeType = LanguageProperty;
    else
    {
      // If new attribute and no value field at all - error
      if (attrTypeFromDb == NoAttributeType)
      {
        orionldError(OrionldBadRequestData, "Missing value/object/languageMap field for Attribute at creation time", attrP->name, 400);
        return false;
      }
    }

    if (valueAndTypeCheck(attrP, attributeType, attrTypeFromDb != NoAttributeType) == false)
      LM_RE(false, ("valueAndTypeCheck failed"));

    if ((attrTypeFromDb != NoAttributeType) && (attributeType != NoAttributeType) && (attributeType != attrTypeFromDb))
    {
      const char* title = attrTypeChangeTitle(attrTypeFromDb, attributeType);
      orionldError(OrionldBadRequestData, title, attrP->name, 400);
      return false;
    }
  }

  KjNode* fieldP = attrP->value.firstChildP;
  KjNode* next;

  while (fieldP != NULL)
  {
    next = fieldP->next;

    if (fieldP->type == KjNull)  // Pure JSON null ... ?   OR, was it a JDON-LD null object that has been translated already?
    {
      if ((orionldState.serviceP->options & ORIONLD_SERVICE_OPTION_ACCEPT_JSONLD_NULL) == 0)
      {
        orionldError(OrionldBadRequestData, "null is not allowed as RHS in a JSON-LD document", fieldP->name, 400);
        return false;
      }
      fieldP = next;
      continue;
    }

    // JSON-LD null ?
    if (fieldP->type == KjObject)
    {
      KjNode* typeP = kjLookup(fieldP, "@type");

      if ((typeP != NULL) && (kjJsonldNullObject(fieldP, typeP) == true))
        typeP->type = KjNull;
    }

    if (fieldP->type == KjNull)  // JSON-LD null
    {
      if ((orionldState.serviceP->options & ORIONLD_SERVICE_OPTION_ACCEPT_JSONLD_NULL) == 0)
      {
        orionldError(OrionldBadRequestData, "RHS as NULL not allowed", fieldP->name, 400);
        return false;
      }

      fieldP = next;
      continue;  // NULL, all good. Next!
    }

    if (fieldP == typeP)
    {
      // The value/object/languageMap is left as is
    }
    else if (strcmp(fieldP->name, "value") == 0)
    {
      if (fieldP->type == KjArray)
      {
        if ((fieldP->value.firstChildP != NULL) && (fieldP->value.firstChildP->next == NULL))
        {
          fieldP->lastChild = fieldP->value.firstChildP->lastChild;  // Might be an array or object inside the array ...
          fieldP->type      = fieldP->value.firstChildP->type;
          fieldP->value     = fieldP->value.firstChildP->value;
        }
      }
      if ((attributeType == Relationship) || (attributeType == LanguageProperty))
      {
        orionldError(OrionldBadRequestData, "Invalid member /value/", "valid for Property/GeoProperty attributes only", 400);
        return false;
      }
    }
    else if (strcmp(fieldP->name, "type") == 0)
    {
      // Sub-attribute type, or GeoProperty-type inside value
    }
    else if (strcmp(fieldP->name, "object") == 0)
    {
      if (attributeType == Relationship)
      {
        if ((objectCheck(fieldP) == false) && (stringArrayCheck(fieldP) == false))  // Until datasetId is fully implemented - string array allowed for Relationship
          return false;
      }
      else
      {
        orionldError(OrionldBadRequestData, "Invalid member /object/", "valid for Relationship attributes only", 400);
        return false;
      }
    }
    else if ((attributeType == LanguageProperty) && (strcmp(fieldP->name, "languageMap") == 0))
    {
      if (languageMapCheck(fieldP) == false)
        return false;
    }
    else if (strcmp(fieldP->name, "observedAt") == 0)
    {
      if (timestampCheck(fieldP) == false)
        return false;
    }
    else if ((isAttribute == true) && (strcmp(fieldP->name, "datasetId") == 0))
    {
      if (datasetIdCheck(fieldP) == false)
        return false;
    }
    else if (strcmp(fieldP->name, "unitCode") == 0)
    {
      if (((attributeType == Property) || (attributeType == NoAttributeType)) && ((attrTypeFromDb == Property) || (attrTypeFromDb == NoAttributeType)))
      {
        if (unitCodeCheck(fieldP) == false)
          return false;
      }
      else
      {
        orionldError(OrionldBadRequestData, "Invalid member /unitCode/", "valid for Property attributes only", 400);
        return false;
      }
    }
    else
    {
      if (pCheckAttribute(fieldP, false, NoAttributeType, false, NULL) == false)
        return false;
    }

    //
    // Add the Attribute Type field, if not present, and, if fully known!
    //
    if ((typeP == NULL) && (attributeType != NoAttributeType))
    {
      typeP = kjString(orionldState.kjsonP, "type", orionldAttributeTypeName(attributeType));
      kjChildAdd(attrP, typeP);
    }

    fieldP = next;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// validAttrName -
//
static bool validAttrName(const char* attrName, bool isAttribute)
{
  bool  ok = true;

  if (isAttribute == true)
  {
    if      (strcmp(attrName, "id")    == 0)  ok = false;
    else if (strcmp(attrName, "@id")   == 0)  ok = false;
    else if (strcmp(attrName, "type")  == 0)  ok = false;
    else if (strcmp(attrName, "@type") == 0)  ok = false;
    else if (strcmp(attrName, "scope") == 0)  ok = false;
  }
  else
  {
    if      (strcmp(attrName, "type")  == 0)  ok = false;
    else if (strcmp(attrName, "@type") == 0)  ok = false;
  }

  if (ok == false)
  {
    const char* title = (isAttribute == true)? "Forbidden attribute name" : "Forbidden sub-attribute name";
    orionldError(OrionldInternalError, title, attrName, 400);
    return false;
  }

  return true;
}



// -----------------------------------------------------------------------------
//
// pCheckAttribute -
//
// PARAMETERS
// attrTypeFromDb -    needed, only for PATCH Entity/Attribute, to make sure
//                     the attribute update isn't trying to modify the type of the attribute.
//                     API endpoints other than those two need not make this check as attributes are REPLACED.
//                     Likewise, in the second (recursive) call to pCheckAttribute for PATCH Attribute, it is not
//                     needed as all sub-attributes are REPLACED.
//
// -----------------------------------------------------------------------------
//
// With Simplified Format, an attribute RHS can be in a variety of formats:
//
// 'attrP' is the output from the parsing step. It's a tree of KjNode, the tree to amend (add missing type if need be) and check for validity.
//
// attrP->value can be:
//   * "A String"
//   * 12
//   * 3.14
//   * true/false
//   * [ 1, 2 ]
//   * {}
//
// o If 'attrP->type' is a SIMPLE FORMAT (non-array, non-object), it's quite straightforward.
//   - Just create a KjNode named "value" and make the entire 'attrP' the RHS of "value"
//     - One exception - if it's a string and the value starts with "urn:ngsi-ld", then it is assumed it's a Relationship
//   - Also, create a KjNode named "type" and set it to "Property" or "Relationship
//   - Then transform 'attrP' into an KjObject and add the "type" and "value" KjNodes to it.
//
// o If 'attrP->type' is an ARRAY, there are two possibilities:
//   - "multi-attributes" - an ARRAY of OBJECTS where at most ONE of the object may lack the datasetId member
//   - RHS of "value" and the same procedure as for SIMPLE formats is performed
//
// o If 'attrP->type' is an OBJECT:
//   - if "type" is present:
//     - and it's a valid NGSI-LD Attribute type name ("Property", "Relationship, "GeoProperty", ...)
//       then the type of the attribute is DECIDED
//     - else ("type" is not a valid type) AND "coordinates" present, and nothing else, it's considered a GeoProperty - procedure as for SIMPLE formats
//   - if "type" is NOT present:
//     - if "value/object/languageMap" is present, then we can deduct the attribute type and add it to the object. Sub-attrs are processed
//     - if no value is present, then no type is added, only sub-attrs are processed.
//
// Or, as source code:
//   if (attrP->type == KjArray)
//     pCheckArrayAttribute(attrP)
//
// o special attributes/sub-attributes are left untouched. They are only checked for validity
//   - Entities have the special attributes:
//       - id
//       - type
//       - scope
//     These are not processed, only checked for validity
//
//     Entities also have three special GeoProperties:
//       - location
//       - observationSpace
//       - operationSpace
//     These are both processed and checked for validity - must be GeoProperty!
//
//  - Attributes of type Property can have the following special attributes:
//      - type
//      - value
//      - observedAt
//      - unitCode
//      - datasetId (sub-attributes don't have datasetId)
//
//  - Attributes of type GeoProperty can have the following special attributes:
//      - type
//      - value
//      - observedAt
//      - datasetId (sub-attributes don't have datasetId)
//
//  - Attributes of type Relationship can have the following special attributes:
//      - type
//      - object
//      - observedAt
//      - datasetId (sub-attributes don't have datasetId)
//
//  - Attributes of type LanguageProperty can have the following special attributes:
//      - type
//      - languageMap
//      - observedAt
//      - datasetId (sub-attributes don't have datasetId)
//
bool pCheckAttribute
(
  KjNode*                 attrP,
  bool                    isAttribute,
  OrionldAttributeType    attrTypeFromDb,
  bool                    attrNameAlreadyExpanded,
  OrionldContextItem*     attrContextInfoP          // Attr Info from the @context (add shortname to struct?)
)
{
  if (attrNameAlreadyExpanded == false)
  {
    if (pCheckName(attrP->name) == false)
      return false;
    if (pCheckUri(attrP->name, attrP->name, false) == false)  // FIXME: Both pCheckName and pCheckUri check for forbidden chars ...
      return false;

    if (isAttribute)
      attrP->name = orionldAttributeExpand(orionldState.contextP, attrP->name, true, &attrContextInfoP);
    else
      attrP->name = orionldSubAttributeExpand(orionldState.contextP, attrP->name, true, &attrContextInfoP);
  }

  if (pCheckTypeFromContext(attrP, attrContextInfoP) == false)
    return false;

  // Invalid name for attribute/sub-attribute?
  if (validAttrName(attrP->name, isAttribute) == false)
    return false;

  if ((isAttribute == true) && (attrP->type == KjArray))
  {
    bool error = false;

    if (multiAttributeArray(attrP, &error) == true)
    {
      for (KjNode* aInstanceP = attrP->value.firstChildP; aInstanceP != NULL; aInstanceP = aInstanceP->next)
      {
        // Do I need the attribute instance in the DB?
        // - only need it to determine the type of the attribute (I think ...)
        // If so, the "non-datasetId" attribute is sufficient
        // - What if there is no "non-datasetId" attr ... ?
        // - Might need the datasetsP anyway
        // - Would be good also (set to NULL in recursive calls, to avoid enter here again - datasetId check)
        // - Actually, cannot be an Array inside there, so that problem is already taken care of
        //
        // => KjNode* datasetsP should be a parameter to this function
        //
        aInstanceP->name = attrP->name;
        if (pCheckAttribute(aInstanceP, true, attrTypeFromDb, attrNameAlreadyExpanded, attrContextInfoP) == false)
          return false;
      }

      return true;
    }
    else
    {
      if (error == true)  // Early detection of erroneous datasetId-array
        return false;
    }
  }

  //
  // Check for special attributes
  //
  // Attributes
  //   If isAttribute == true, we're on Entity Level. There are 3 special attributes, all GeoProperty
  //   As we don't know the type yet, that check is postponed to pCheckAttributeObject
  //
  // Sub-Attributes
  //   If isAttribute == false, we're on Attribute level. This is even more complex.
  //   We don't know which sub-attributes are special until we know the type of the attribute (Property, Relationship, etc)
  //   Postponed to ... all simple Property functions - pCheckAttribute[String|Integer|Float|Boolean] + pCheckAttributeObject (Geo)
  //
  if      (attrP->type == KjString)  return pCheckAttributeString(attrP,  isAttribute, attrTypeFromDb, attrContextInfoP);
  else if (attrP->type == KjInt)     return pCheckAttributeInteger(attrP, isAttribute, attrTypeFromDb, attrContextInfoP);
  else if (attrP->type == KjFloat)   return pCheckAttributeFloat(attrP,   isAttribute, attrTypeFromDb, attrContextInfoP);
  else if (attrP->type == KjBoolean) return pCheckAttributeBoolean(attrP, isAttribute, attrTypeFromDb, attrContextInfoP);
  else if (attrP->type == KjArray)   return pCheckAttributeArray(attrP,   isAttribute, attrTypeFromDb, attrContextInfoP);
  else if (attrP->type == KjObject)  return pCheckAttributeObject(attrP,  isAttribute, attrTypeFromDb, attrContextInfoP);
  else if (attrP->type == KjNull)    return pCheckAttributeNull(attrP);

  // Invalid JSON type of the attribute - we should never reach this point
  orionldError(OrionldInternalError, "invalid value type for attribute", attrP->name, 500);

  return false;
}