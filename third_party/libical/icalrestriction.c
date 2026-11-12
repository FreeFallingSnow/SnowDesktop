/*======================================================================
 File: icalrestriction.c

 SPDX-FileCopyrightText: 2000, Eric Busboom  <eric@civicknowledge.com>
 SPDX-License-Identifier: LGPL-2.1-only OR MPL-2.0
 ======================================================================*/

/**
 * @file icalrestriction.c
 * @brief Functions to check if an icalcomponent meets the restrictions
 * imposed by the standard.
 */

/*#line 7 "icalrestriction.c.in"*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "icalrestriction.h"
#include "icalerror_p.h"
#include "icalmemory.h"

/* Define the structs for the restrictions. these data are filled out
in machine generated code below */

struct icalrestriction_record;

typedef const char *(*restriction_func) (const struct icalrestriction_record * rec,
                                         icalcomponent *comp, icalproperty *prop);

typedef struct icalrestriction_record
{
    icalproperty_method method;
    icalcomponent_kind component;
    icalproperty_kind property;
    icalcomponent_kind subcomponent;
    icalrestriction_kind restriction;
    restriction_func function;
} icalrestriction_record;

static const icalrestriction_record *icalrestriction_get_restriction(
    const icalrestriction_record *start,
    icalproperty_method method, icalcomponent_kind component,
    icalproperty_kind property, icalcomponent_kind subcomp);

const icalrestriction_record null_restriction_record =
    { ICAL_METHOD_NONE, ICAL_NO_COMPONENT,
      ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_UNKNOWN, NULL };

/** Each row gives the result of comparing a restriction against a count.
   The columns in each row represent 0,1,2+. '-1' indicates
   'invalid, 'don't care' or 'needs more analysis' So, for
   ICAL_RESTRICTION_ONE, if there is 1 of a property with that
   restriction, it passes, but if there are 0 or 2+, it fails. */

static const bool compare_map[ICAL_RESTRICTION_UNKNOWN + 1][3] = {
    {true, true, true},   /*ICAL_RESTRICTION_NONE */
    {true, false, false}, /*ICAL_RESTRICTION_ZERO */
    {false, true, false}, /*ICAL_RESTRICTION_ONE */
    {true, true, true},   /*ICAL_RESTRICTION_ZEROPLUS */
    {false, true, true},  /*ICAL_RESTRICTION_ONEPLUS */
    {true, true, false},  /*ICAL_RESTRICTION_ZEROORONE */
    {true, true, false},  /*ICAL_RESTRICTION_ONEEXCLUSIVE */
    {true, true, false},  /*ICAL_RESTRICTION_ONEMUTUAL */
    {true, true, true}    /*ICAL_RESTRICTION_UNKNOWN */
};

static const char restr_string_map[ICAL_RESTRICTION_UNKNOWN + 1][60] = {
    "unknown number", /*ICAL_RESTRICTION_NONE */
    "0", /*ICAL_RESTRICTION_ZERO */
    "1", /*ICAL_RESTRICTION_ONE */
    "zero or more", /*ICAL_RESTRICTION_ZEROPLUS */
    "one or more", /*ICAL_RESTRICTION_ONEPLUS */
    "zero or one", /*ICAL_RESTRICTION_ZEROORONE */
    "zero or one, exclusive with another property", /*ICAL_RESTRICTION_ONEEXCLUSIVE */
    "zero or one, mutual with another property", /*ICAL_RESTRICTION_ONEMUTUAL */
    "unknown number"    /*ICAL_RESTRICTION_UNKNOWN */
};

bool icalrestriction_compare(icalrestriction_kind restr, int count)
{
    /* restr is an unsigned int and ICAL_RESTRICTION_NONE == 0,
       so no need to check if restr < ICAL_RESTRICTION_NONE */
    if (restr > ICAL_RESTRICTION_UNKNOWN || count < 0) {
        return false;
    }

    if (count > 2) {
        count = 2;
    }

    return compare_map[restr][count];
}

/* Special case routines */

static const char *icalrestriction_validate_status_value(
    const icalrestriction_record *rec, icalcomponent *comp, icalproperty *prop)
{
    icalproperty_status stat;

    _unused(comp);

    if (!prop) {
        return 0;
    }

    stat = icalproperty_get_status(prop);

    if (rec->method == ICAL_METHOD_CANCEL) {
        switch (rec->component) {
        case ICAL_VEVENT_COMPONENT:
        case ICAL_VTODO_COMPONENT:
            /* Hack. see rfc5546, 3.2.5 CANCEL for property STATUS. I don't
               understand the note */
            break;

        case ICAL_VJOURNAL_COMPONENT:
        case ICAL_VPOLL_COMPONENT:
            if (stat != ICAL_STATUS_CANCELLED) {
                return
                    "Failed iTIP restrictions for STATUS property. "
                    "Value must be CANCELLED";
            }
            break;

        default:
            break;
        }
    } else {
        switch (rec->component) {
        case ICAL_VEVENT_COMPONENT:
            switch (rec->method) {
            case ICAL_METHOD_PUBLISH:
            case ICAL_METHOD_COUNTER:
                if (!(stat == ICAL_STATUS_TENTATIVE ||
                      stat == ICAL_STATUS_CONFIRMED ||
                      stat == ICAL_STATUS_CANCELLED)) {
                    return
                        "Failed iTIP restrictions for STATUS property. "
                        "Value must be one of TENTATIVE, CONFIRMED or CANCELED";
                }
                break;

            case ICAL_METHOD_REQUEST:
            case ICAL_METHOD_ADD:
                if (!(stat == ICAL_STATUS_TENTATIVE ||
                      stat == ICAL_STATUS_CONFIRMED)) {
                    return
                        "Failed iTIP restrictions for STATUS property. "
                        "Value must be one of TENTATIVE or CONFIRMED";
                }
                break;

            default:
                break;
            }
            break;

        case ICAL_VTODO_COMPONENT:
            switch (rec->method) {
            case ICAL_METHOD_REQUEST:
            case ICAL_METHOD_ADD:
            case ICAL_METHOD_COUNTER:
                if (!(stat == ICAL_STATUS_COMPLETED ||
                      stat == ICAL_STATUS_NEEDSACTION ||
                      stat == ICAL_STATUS_INPROCESS)) {
                    return
                        "Failed iTIP restrictions for STATUS property. "
                        "Value must be one of COMPLETED, NEEDS-ACTION or IN-PROCESS";
                }
                break;

            default:
                break;
            }
            break;

        default:
            break;
        }
    }

    return 0;
}

static const char *icalrestriction_must_be_recurring(const icalrestriction_record * rec,
                                                     icalcomponent *comp, icalproperty *prop)
{
    _unused(rec);
    _unused(comp);
    _unused(prop);
    return 0;
}

const char *icalrestriction_must_if_tz_ref(const icalrestriction_record * rec,
                                           icalcomponent *comp, icalproperty *prop)
{
    _unused(rec);
    _unused(comp);
    _unused(prop);
    return 0;
}

static const char *_validate_duration(icalcomponent *comp, icalproperty *prop)
{
    struct icaldurationtype duration;

    if (!icalcomponent_get_first_property(comp, ICAL_DTSTART_PROPERTY)) {
        return
            "Failed iTIP restrictions for DURATION property. "
            "The component must have DTSTART";
    }

    duration = icalproperty_get_duration(prop);

    if (icaldurationtype_as_utc_seconds(duration) < 0) {
        /* NOTE: Per RFC 5545, Section 3.8.2.5, DURATION > 0,
           but DURATION == 0 occurs frequently enough in the wild
           for us to allow it */
        return
            "Failed iTIP restrictions for DURATION property. "
            "The DURATION value must be non-negative";
    }

    return 0;
}

/* This function is called with comp=VEVENT, prop=DURATION */
static const char *icalrestriction_no_dtend(const icalrestriction_record * rec,
                                            icalcomponent *comp, icalproperty *prop)
{
    _unused(rec);

    if (prop == NULL)
        return 0;

    if (icalcomponent_get_first_property(comp, ICAL_DTEND_PROPERTY)) {
        return
            "Failed iTIP restrictions for DURATION property. "
            "The component must not have both DURATION and DTEND";
    }

    return _validate_duration(comp, prop);
}

/* This function is called with comp=VTODO, prop=DURATION */
static const char *icalrestriction_no_due(const icalrestriction_record * rec,
                                          icalcomponent *comp, icalproperty *prop)
{
    _unused(rec);

    if (prop == NULL)
        return 0;

    if (icalcomponent_get_first_property(comp, ICAL_DUE_PROPERTY)) {
        return
            "Failed iTIP restrictions for DURATION property. "
            "The component must not have both DURATION and DUE";
    }

    return _validate_duration(comp, prop);
}

#define TMP_BUF_SIZE 1024

/* This function is called with either comp=VEVENT, prop=DTEND or
   comp=VTODO, prop=DUE */
static const char *icalrestriction_no_duration(const icalrestriction_record * rec,
                                               icalcomponent *comp, icalproperty *prop)
{
    const char *pkind;
    icalproperty *dtstartp;
    icaltimetype dtstart;
    icaltimetype dtend_due;
    bool dtstart_islocal;
    bool dtend_due_islocal;
    _unused(rec);

    if (prop == NULL)
        return 0;

    pkind = icalproperty_kind_to_string(icalproperty_isa(prop));

    if (icalcomponent_get_first_property(comp, ICAL_DURATION_PROPERTY)) {
        char *temp = (char*) icalmemory_tmp_buffer(TMP_BUF_SIZE);
        (void)snprintf(temp, TMP_BUF_SIZE,
                       "Failed iTIP restrictions for %s property. "
                       "The component must not have both %s and DURATION",
                       pkind, pkind);
        return temp;
    }

    /* Per RFC 5545, Sections 3.8.2.2 and 3.8.2.3:
       The value type of the DTEND/DUE property MUST be the same as the
       DTSTART property, and its value MUST be later in time than the value
       of the DTSTART property.
       Furthermore, the DTEND/DUE property MUST be specified as a date
       with local time if and only if the DTSTART property is also
       specified as a date with local time.
    */
    dtstartp = icalcomponent_get_first_property(comp, ICAL_DTSTART_PROPERTY);

    if (!dtstartp) {
        /* DTSTART is optional in VTODO */
        return (icalcomponent_isa(comp) == ICAL_VTODO_COMPONENT) ? 0 :
            "Failed iTIP restrictions for DTEND property. "
            "The component must have DTSTART";
    }

    dtstart = icalproperty_get_datetime_with_component(dtstartp, comp);
    dtend_due = icalproperty_get_datetime_with_component(prop, comp);

    if (icaltime_is_date(dtend_due) != icaltime_is_date(dtstart)) {
        char *temp = (char*) icalmemory_tmp_buffer(TMP_BUF_SIZE);
        (void)snprintf(temp, TMP_BUF_SIZE,
                       "Failed iTIP restrictions for %s property. "
                       "%s must have the same value type as DTSTART",
                       pkind, pkind);
        return temp;
    }

    dtstart_islocal = !dtstart.zone &&
        !icalproperty_get_first_parameter(dtstartp, ICAL_TZID_PARAMETER);
    dtend_due_islocal = !dtend_due.zone &&
        !icalproperty_get_first_parameter(prop, ICAL_TZID_PARAMETER);

    if (dtend_due_islocal != dtstart_islocal) {
        char *temp = (char*) icalmemory_tmp_buffer(TMP_BUF_SIZE);
        (void)snprintf(temp, TMP_BUF_SIZE,
                       "Failed iTIP restrictions for %s property. "
                       "%s must be local time if DTSTART is local time",
                       pkind, pkind);
        return temp;
    }

    if (icaltime_compare(dtend_due, dtstart) < 0) {
        /* NOTE: Per RFC 5545, DTEND != DTSTART, but this occurs
           frequently enough in the wild for us to allow it */
        char *temp = (char*) icalmemory_tmp_buffer(TMP_BUF_SIZE);
        (void)snprintf(temp, TMP_BUF_SIZE,
                       "Failed iTIP restrictions for %s property. "
                       "%s must occur after DTSTART",
                       pkind, pkind);
        return temp;
    }

    return 0;
}

static bool _check_restriction(icalcomponent *comp,
                               const icalrestriction_record *record,
                               int count, icalproperty *prop)
{
    icalrestriction_kind restr;
    const char *funcr = 0;
    bool compare;

    restr = record->restriction;

    if (restr == ICAL_RESTRICTION_ONEEXCLUSIVE ||
        restr == ICAL_RESTRICTION_ONEMUTUAL) {

        /* First treat is as a 0/1 restriction */
        restr = ICAL_RESTRICTION_ZEROORONE;
    }
    if (restr > ICAL_RESTRICTION_UNKNOWN) {
        restr = ICAL_RESTRICTION_UNKNOWN;
    }

    compare = icalrestriction_compare(restr, count);

    if (!compare) {
        icalproperty *errProp;
        icalparameter *errParam;
        const char *type, *kind;
        char *temp;

        if (record->subcomponent != ICAL_NO_COMPONENT) {
            type = "component";
            kind = icalcomponent_kind_to_string(record->subcomponent);
        } else {
            type = "property";
            kind = icalproperty_kind_to_string(record->property);
        }

        /* Don't worry if the error message is truncated at TMP_BUF_SIZE chars */
        temp = (char*) icalmemory_tmp_buffer(TMP_BUF_SIZE);
        (void)snprintf(temp, TMP_BUF_SIZE,
                       "Failed iTIP restrictions for %s %s. "
                       "Expected %s instances of the %s and got %d",
                       kind, type, restr_string_map[restr], type, count);
        errParam = icalparameter_new_xlicerrortype(ICAL_XLICERRORTYPE_INVALIDITIP);
        errProp = icalproperty_vanew_xlicerror(temp, errParam, (void *)0);
        icalcomponent_add_property(comp, errProp);
        icalproperty_free(errProp);
    }

    if (record->function != NULL) {
        funcr = record->function(record, comp, prop);
    }

    if (funcr != 0) {
        icalproperty *errProp;
        icalparameter *errParam;

        /* coverity[resource_leak] */
        errParam = icalparameter_new_xlicerrortype(ICAL_XLICERRORTYPE_INVALIDITIP);
        errProp = icalproperty_vanew_xlicerror(funcr, errParam, (void *)0);
        icalcomponent_add_property(comp, errProp);
        icalproperty_free(errProp);

        compare = false;
    }

    return compare;
}

static bool icalrestriction_check_component(icalproperty_method method,
                                            icalcomponent *comp)
{
    icalcomponent_kind comp_kind, inner_kind;
    icalproperty_kind prop_kind;
    const icalrestriction_record *start_record;
    icalproperty *method_prop = NULL;
    icalcomponent *inner_comp;
    const char *errStr = NULL;
    int count;
    bool compare;
    bool valid = true;

    comp_kind = icalcomponent_isa(comp);

    switch (comp_kind) {
    case ICAL_VCALENDAR_COMPONENT:
        if (!icalcomponent_get_first_real_component(comp)) {

            errStr = "Failed iTIP restrictions for VCALENDAR component. "
                "Expected one or more \"real\" sub-components and got 0";
        }

        /* Get the Method property from the component */
        method_prop = icalcomponent_get_first_property(comp, ICAL_METHOD_PROPERTY);
        break;

    case ICAL_VTIMEZONE_COMPONENT:
        if (!icalcomponent_get_first_component(comp, ICAL_XSTANDARD_COMPONENT) &&
            !icalcomponent_get_first_component(comp, ICAL_XDAYLIGHT_COMPONENT)) {

            errStr = "Failed iTIP restrictions for VTIMEZONE component. "
                "Expected one or more STANDARD/DAYLIGHT sub-components and got 0";
        }

        method = ICAL_METHOD_NONE;
        break;

    default:
        break;
    }

    if (errStr != NULL) {
        icalproperty *errProp;
        icalparameter *errParam;

        /* coverity[resource_leak] */
        errParam = icalparameter_new_xlicerrortype(ICAL_XLICERRORTYPE_INVALIDITIP);
        errProp = icalproperty_vanew_xlicerror(errStr, errParam, (void *)0);
        icalcomponent_add_property(comp, errProp);
        icalproperty_free(errProp);

        valid = false;
    }

    /* Check all of the properties in this component */

    start_record = icalrestriction_get_restriction(NULL, method, comp_kind,
                                                   ICAL_ANY_PROPERTY,
                                                   ICAL_NO_COMPONENT);

    if (start_record != &null_restriction_record) {

        for (prop_kind = ICAL_ANY_PROPERTY + 1;
             prop_kind != ICAL_NO_PROPERTY; prop_kind++) {

            const icalrestriction_record *record =
                icalrestriction_get_restriction(start_record, method, comp_kind,
                                                prop_kind, ICAL_NO_COMPONENT);

            icalproperty *prop =
                icalcomponent_get_first_property(comp, prop_kind);

            count = icalcomponent_count_properties(comp, prop_kind);

            compare = _check_restriction(comp, record, count, prop);

            valid = valid && compare;
        }
    }

    /* Now check the inner components */

    start_record = icalrestriction_get_restriction(start_record, method, comp_kind,
                                                   ICAL_NO_PROPERTY,
                                                   ICAL_ANY_COMPONENT);

    if (start_record != &null_restriction_record) {

        for (inner_kind = ICAL_NO_COMPONENT + 3;
             inner_kind != ICAL_NUM_COMPONENT_TYPES; inner_kind++) {

            const icalrestriction_record *record =
                icalrestriction_get_restriction(start_record, method, comp_kind,
                                                ICAL_NO_PROPERTY, inner_kind);

            count = icalcomponent_count_components(comp, inner_kind);

            compare = _check_restriction(comp, record, count, NULL);

            valid = valid && compare;
        }
    }

    if (method_prop == 0) {
        method = ICAL_METHOD_NONE;
    } else {
        method = icalproperty_get_method(method_prop);
    }

    for (inner_comp = icalcomponent_get_first_component(comp, ICAL_ANY_COMPONENT);
         inner_comp != 0;
         inner_comp = icalcomponent_get_next_component(comp, ICAL_ANY_COMPONENT)) {

        compare = icalrestriction_check_component(method, inner_comp);

        valid = valid && compare;
    }

    return valid;
}

bool icalrestriction_check(icalcomponent *outer_comp)
{
    icalcomponent_kind comp_kind;
    bool valid;

    icalerror_check_arg_rz((outer_comp != 0), "outer comp");

    comp_kind = icalcomponent_isa(outer_comp);

    if (comp_kind != ICAL_VCALENDAR_COMPONENT) {
        icalerror_set_errno(ICAL_BADARG_ERROR);
        return 0;
    }

    /* Check the VCALENDAR wrapper */
    valid = (bool)icalrestriction_check_component(ICAL_METHOD_NONE, outer_comp);

    return valid;
}

static const char *icalrestriction_validate_valarm_prop(
    const icalrestriction_record *rec, icalcomponent *comp, icalproperty *prop)
{
    icalrestriction_record record =
        { ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT,
          rec->property, ICAL_NO_COMPONENT, ICAL_RESTRICTION_UNKNOWN, NULL };
    const icalrestriction_record *myrec = NULL;
    enum icalproperty_action action = ICAL_ACTION_NONE;
    icalproperty *action_prop;
    int count = 0;

    switch (rec->subcomponent) {
    case ICAL_NO_COMPONENT:
        action_prop = icalcomponent_get_first_property(comp, ICAL_ACTION_PROPERTY);

        if (action_prop) {
            action = icalproperty_get_action(action_prop);
        }

        if (prop) {
            if (rec->restriction == ICAL_RESTRICTION_ZEROPLUS ||
                rec->restriction == ICAL_RESTRICTION_ONEPLUS) {
                count = icalcomponent_count_properties(comp, rec->property);
            } else {
                count = 1;
            }
        }

        switch (rec->property) {
        case ICAL_DURATION_PROPERTY:
            if (count &&
                !icalcomponent_get_first_property(comp, ICAL_DURATION_PROPERTY)) {
                return
                    "Failed iTIP restrictions for REPEAT property. "
                    "This component must have a REPEAT property "
                    "if it has a DURATION property";
            }
            break;

        case ICAL_REPEAT_PROPERTY:
            if (count &&
                !icalcomponent_get_first_property(comp, ICAL_DURATION_PROPERTY)) {
                return
                    "Failed iTIP restrictions for DURATION property. "
                    "This component must have a DURATION property "
                    "if it has a REPEAT property";
            }
            break;

        case ICAL_ATTACH_PROPERTY:
            if (count) {
                switch (action) {
                case ICAL_ACTION_AUDIO:
                case ICAL_ACTION_PROCEDURE:
                    record.restriction = ICAL_RESTRICTION_ZEROORONE;
                    myrec = &record;
                    break;

                case ICAL_ACTION_DISPLAY:
                    record.restriction = ICAL_RESTRICTION_ZERO;
                    myrec = &record;
                    break;

                default:
                    break;
                }
                break;
            }
            break;

        case ICAL_ATTENDEE_PROPERTY:
            switch (action) {
            case ICAL_ACTION_AUDIO:
            case ICAL_ACTION_DISPLAY:
            case ICAL_ACTION_PROCEDURE:
                if (count) {
                    record.restriction = ICAL_RESTRICTION_ZERO;
                    myrec = &record;
                }
                break;

            case ICAL_ACTION_EMAIL:
                if (!count) {
                    record.restriction = ICAL_RESTRICTION_ONEPLUS;
                    myrec = &record;
                }
                break;

            default:
                break;
            }
            break;

        case ICAL_DESCRIPTION_PROPERTY:
            switch (action) {
            case ICAL_ACTION_AUDIO:
                if (count) {
                    record.restriction = ICAL_RESTRICTION_ZERO;
                    myrec = &record;
                }
                break;

            case ICAL_ACTION_DISPLAY:
            case ICAL_ACTION_EMAIL:
                if (!count) {
                    record.restriction = ICAL_RESTRICTION_ONE;
                    myrec = &record;
                }
                break;

            default:
                break;
            }
            break;

        case ICAL_SUMMARY_PROPERTY:
            switch (action) {
            case ICAL_ACTION_AUDIO:
            case ICAL_ACTION_DISPLAY:
            case ICAL_ACTION_PROCEDURE:
                if (count) {
                    record.restriction = ICAL_RESTRICTION_ZERO;
                    myrec = &record;
                }
                break;

            case ICAL_ACTION_EMAIL:
                if (!count) {
                    record.restriction = ICAL_RESTRICTION_ONE;
                    myrec = &record;
                }
                break;

            default:
                break;
            }
            break;

        default:
            break;
        }
        break;

    case ICAL_VLOCATION_COMPONENT:
        if (!icalcomponent_get_first_property(comp, ICAL_PROXIMITY_PROPERTY)) {
            return
                "Failed iTIP restrictions for VLOCATION component. "
                "This component must only appear in a VALARM component "
                "if the VALARM has a PROXIMITY property.";
        }
        break;

    default:
        break;
    }

    if (myrec) {
        (void)_check_restriction(comp, myrec, count, NULL);
    }

    return 0;
}

static const icalrestriction_record icalrestriction_records[] = {
    {ICAL_METHOD_ADD, ICAL_VAGENDA_COMPONENT, ICAL_CALMASTER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VAGENDA_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VAGENDA_COMPONENT, ICAL_OWNER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VAGENDA_COMPONENT, ICAL_RELCALID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VAGENDA_COMPONENT, ICAL_TZID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_COLOR_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_CONCEPT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_CONFERENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_DTEND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_duration},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_dtend},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_IMAGE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_LINK_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_PARTICIPANT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VFREEBUSY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VJOURNAL_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VLOCATION_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VRESOURCE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, icalrestriction_must_if_tz_ref},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTODO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, icalrestriction_must_be_recurring},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_REFID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_RELCALID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_validate_status_value},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_STRUCTUREDDATA_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_STYLEDDESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_TRANSP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VEVENT_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_COLOR_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_IMAGE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_PARTICIPANT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VEVENT_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VFREEBUSY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VLOCATION_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VRESOURCE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTODO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_STRUCTUREDDATA_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_STYLEDDESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VJOURNAL_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_COLOR_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_CONCEPT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_CONFERENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_DUE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_no_duration},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_no_due},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_IMAGE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_LINK_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_PARTICIPANT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VEVENT_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VFREEBUSY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VJOURNAL_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VLOCATION_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VRESOURCE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_PERCENTCOMPLETE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, icalrestriction_must_be_recurring},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_REFID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_RELCALID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_validate_status_value},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_STRUCTUREDDATA_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_STYLEDDESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_ADD, ICAL_VTODO_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VAGENDA_COMPONENT, ICAL_CALMASTER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VAGENDA_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VAGENDA_COMPONENT, ICAL_OWNER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VAGENDA_COMPONENT, ICAL_RELCALID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VAGENDA_COMPONENT, ICAL_TZID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_COLOR_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_CONCEPT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_CONFERENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_DTEND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_duration},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_dtend},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_IMAGE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_LINK_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_PARTICIPANT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VFREEBUSY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VJOURNAL_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VLOCATION_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VRESOURCE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, icalrestriction_must_if_tz_ref},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTODO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_must_be_recurring},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_REFID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_RELCALID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_validate_status_value},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_STRUCTUREDDATA_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_STYLEDDESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_TRANSP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VEVENT_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_COLOR_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_IMAGE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_PARTICIPANT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VEVENT_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VFREEBUSY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VLOCATION_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VRESOURCE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTODO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_must_be_recurring},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_validate_status_value},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_STRUCTUREDDATA_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_STYLEDDESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VJOURNAL_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_ACCEPTRESPONSE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_COMPLETED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_DTEND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_duration},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_dtend},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VAVAILABILITY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VEVENT_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VFREEBUSY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VJOURNAL_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, icalrestriction_must_if_tz_ref},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTODO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VVOTER_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_POLLCOMPLETION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_POLLITEMID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_POLLMODE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_POLLPROPERTIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_validate_status_value},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_TRANSP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VPOLL_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_COLOR_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_CONCEPT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_CONFERENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_DUE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_no_duration},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_no_due},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_IMAGE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_LINK_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_PARTICIPANT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VEVENT_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VFREEBUSY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VLOCATION_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VRESOURCE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_PERCENTCOMPLETE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_must_be_recurring},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_REFID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_RELCALID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_validate_status_value},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_STRUCTUREDDATA_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_STYLEDDESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_CANCEL, ICAL_VTODO_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VAGENDA_COMPONENT, ICAL_CALMASTER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VAGENDA_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VAGENDA_COMPONENT, ICAL_OWNER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VAGENDA_COMPONENT, ICAL_RELCALID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VAGENDA_COMPONENT, ICAL_TZID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_COLOR_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_CONCEPT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_CONFERENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_DTEND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_duration},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_dtend},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_IMAGE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_LINK_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_PARTICIPANT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VFREEBUSY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VJOURNAL_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VLOCATION_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VRESOURCE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, icalrestriction_must_if_tz_ref},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTODO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_must_be_recurring},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_REFID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_RELCALID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_validate_status_value},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_STRUCTUREDDATA_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_STYLEDDESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_TRANSP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VEVENT_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_COLOR_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_CONCEPT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_CONFERENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_DUE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_no_duration},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_no_due},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_IMAGE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_LINK_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_PARTICIPANT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VEVENT_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VFREEBUSY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VLOCATION_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VRESOURCE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_PERCENTCOMPLETE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_must_be_recurring},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_REFID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_RELCALID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_validate_status_value},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_STRUCTUREDDATA_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_STYLEDDESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_COUNTER, ICAL_VTODO_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VAGENDA_COMPONENT, ICAL_CALMASTER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VAGENDA_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VAGENDA_COMPONENT, ICAL_OWNER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VAGENDA_COMPONENT, ICAL_RELCALID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VAGENDA_COMPONENT, ICAL_TZID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_COLOR_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_CONCEPT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_CONFERENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_DTEND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_duration},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_dtend},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_IMAGE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_LINK_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_PARTICIPANT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VFREEBUSY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VJOURNAL_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VLOCATION_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VRESOURCE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZERO, icalrestriction_must_if_tz_ref},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTODO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_must_be_recurring},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_REFID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_RELCALID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_STRUCTUREDDATA_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_STYLEDDESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_TRANSP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VEVENT_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_COLOR_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_CONCEPT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_CONFERENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_DUE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_no_duration},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_no_due},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_IMAGE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_LINK_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_PARTICIPANT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VEVENT_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VFREEBUSY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VLOCATION_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VRESOURCE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_PERCENTCOMPLETE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_must_be_recurring},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_REFID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_RELCALID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_STRUCTUREDDATA_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_STYLEDDESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_DECLINECOUNTER, ICAL_VTODO_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_PARTICIPANT_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_PARTICIPANT_COMPONENT, ICAL_CALENDARADDRESS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_PARTICIPANT_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_PARTICIPANT_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_PARTICIPANT_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_PARTICIPANT_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_PARTICIPANT_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_PARTICIPANT_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_PARTICIPANT_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_PARTICIPANT_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_PARTICIPANT_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_PARTICIPANT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VLOCATION_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_PARTICIPANT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VRESOURCE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_PARTICIPANT_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_PARTICIPANT_COMPONENT, ICAL_PARTICIPANTTYPE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_PARTICIPANT_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_PARTICIPANT_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_PARTICIPANT_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_PARTICIPANT_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_PARTICIPANT_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_PARTICIPANT_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_PARTICIPANT_COMPONENT, ICAL_STRUCTUREDDATA_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_PARTICIPANT_COMPONENT, ICAL_STYLEDDESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_PARTICIPANT_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_PARTICIPANT_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_PARTICIPANT_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_PARTICIPANT_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VAGENDA_COMPONENT, ICAL_ALLOWCONFLICT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VAGENDA_COMPONENT, ICAL_CALMASTER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VAGENDA_COMPONENT, ICAL_DEFAULTCHARSET_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VAGENDA_COMPONENT, ICAL_DEFAULTLOCALE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VAGENDA_COMPONENT, ICAL_DEFAULTTZID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VAGENDA_COMPONENT, ICAL_OWNER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VAGENDA_COMPONENT, ICAL_RELCALID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VAGENDA_COMPONENT, ICAL_TZID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_ACTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, icalrestriction_validate_valarm_prop},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, icalrestriction_validate_valarm_prop},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_CALSCALE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_COMPLETED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_validate_valarm_prop},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_DTEND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_DUE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEMUTUAL, icalrestriction_validate_valarm_prop},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_FREEBUSY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_METHOD_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_NO_PROPERTY, ICAL_VAVAILABILITY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_NO_PROPERTY, ICAL_VEVENT_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_NO_PROPERTY, ICAL_VFREEBUSY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_NO_PROPERTY, ICAL_VJOURNAL_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_NO_PROPERTY, ICAL_VLOCATION_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, icalrestriction_validate_valarm_prop},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_NO_PROPERTY, ICAL_VPOLL_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTODO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_PERCENTCOMPLETE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_PRODID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_REPEAT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEMUTUAL, icalrestriction_validate_valarm_prop},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_validate_valarm_prop},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_TRANSP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_TRIGGER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_TZID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_TZNAME_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_TZOFFSETFROM_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_TZOFFSETTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_TZURL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_VERSION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VALARM_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_ACTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_BUSYTYPE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_CALSCALE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_COMPLETED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_DTEND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_duration},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_DUE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_dtend},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_FREEBUSY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_METHOD_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_NO_PROPERTY, ICAL_XAVAILABLE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_PERCENTCOMPLETE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_PRODID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_REPEAT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_TRANSP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_TRIGGER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_TZID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_TZNAME_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_TZOFFSETFROM_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_TZOFFSETTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_TZURL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_VERSION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VAVAILABILITY_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_ACTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_CALSCALE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_COLOR_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_COMPLETED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_DTEND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_DUE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_FREEBUSY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_IMAGE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_METHOD_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_NAME_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_PERCENTCOMPLETE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_PRODID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_REFRESHINTERVAL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_RELCALID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_REPEAT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_SOURCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_TRANSP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_TRIGGER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_TZID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_TZNAME_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_TZOFFSETFROM_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_TZOFFSETTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_TZURL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_VERSION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VCALENDAR_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_ACTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_CALSCALE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_COLOR_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_COMPLETED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_CONCEPT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_CONFERENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_DTEND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_duration},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_DUE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_dtend},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_FREEBUSY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_IMAGE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_LINK_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_METHOD_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_PARTICIPANT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VLOCATION_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VRESOURCE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_PERCENTCOMPLETE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_PRODID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_must_be_recurring},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_REFID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_RELCALID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_REPEAT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_STRUCTUREDDATA_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_STYLEDDESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_TRANSP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_TRIGGER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_TZID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_TZNAME_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_TZOFFSETFROM_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_TZOFFSETTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_TZURL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_VERSION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VEVENT_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_ACTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_CALSCALE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_COMPLETED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_DTEND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_DUE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_FREEBUSY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_METHOD_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_PARTICIPANT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_VLOCATION_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_VRESOURCE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_PERCENTCOMPLETE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_PRODID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_REPEAT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_STYLEDDESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_TRANSP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_TRIGGER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_TZID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_TZNAME_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_TZOFFSETFROM_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_TZOFFSETTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_TZURL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_VERSION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VFREEBUSY_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_ACTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_CALSCALE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_COLOR_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_COMPLETED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_DTEND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_DUE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_FREEBUSY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_IMAGE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_METHOD_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_PARTICIPANT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VLOCATION_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VRESOURCE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_PERCENTCOMPLETE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_PRODID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_must_be_recurring},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_REPEAT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_STRUCTUREDDATA_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_STYLEDDESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_TRANSP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_TRIGGER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_TZID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_TZNAME_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_TZOFFSETFROM_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_TZOFFSETTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_TZURL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_VERSION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VJOURNAL_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VLOCATION_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VLOCATION_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VLOCATION_COMPONENT, ICAL_LOCATIONTYPE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VLOCATION_COMPONENT, ICAL_NAME_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VLOCATION_COMPONENT, ICAL_STRUCTUREDDATA_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VLOCATION_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VLOCATION_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VPATCH_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VPATCH_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VPATCH_COMPONENT, ICAL_NO_PROPERTY, ICAL_XPATCH_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VPATCH_COMPONENT, ICAL_PATCHORDER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VPATCH_COMPONENT, ICAL_PATCHVERSION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VPATCH_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VPATCH_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_ACCEPTRESPONSE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_COMPLETED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_DTEND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_duration},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_dtend},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VAVAILABILITY_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VEVENT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VFREEBUSY_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VJOURNAL_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, icalrestriction_must_if_tz_ref},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTODO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VVOTER_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_POLLCOMPLETION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_POLLMODE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_POLLPROPERTIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_POLLWINNER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VPOLL_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VQUERY_COMPONENT, ICAL_EXPAND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VQUERY_COMPONENT, ICAL_QUERY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VQUERY_COMPONENT, ICAL_QUERYNAME_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VRESOURCE_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VRESOURCE_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VRESOURCE_COMPONENT, ICAL_NAME_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VRESOURCE_COMPONENT, ICAL_RESOURCETYPE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VRESOURCE_COMPONENT, ICAL_STRUCTUREDDATA_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VRESOURCE_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VRESOURCE_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_ACTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_CALSCALE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_COMPLETED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_DTEND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_DUE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_FREEBUSY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_METHOD_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_NO_PROPERTY, ICAL_XDAYLIGHT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_NO_PROPERTY, ICAL_XSTANDARD_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_PERCENTCOMPLETE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_PRODID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_REPEAT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_TRANSP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_TRIGGER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_TZID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_TZIDALIASOF_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_TZNAME_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_TZOFFSETFROM_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_TZOFFSETTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_TZUNTIL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_TZURL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_VERSION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTIMEZONE_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_ACTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_CALSCALE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_COLOR_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_COMPLETED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_CONCEPT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_CONFERENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_DTEND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_DUE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_duration},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_due},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_FREEBUSY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_IMAGE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_LINK_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_METHOD_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_PARTICIPANT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VLOCATION_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VRESOURCE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_PERCENTCOMPLETE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_PRODID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_must_be_recurring},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_REFID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_RELCALID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_REPEAT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_STRUCTUREDDATA_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_STYLEDDESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_TRANSP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_TRIGGER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_TZID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_TZNAME_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_TZOFFSETFROM_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_TZOFFSETTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_TZURL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_VERSION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VTODO_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VVOTER_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VVOTER_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VVOTER_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VVOTER_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VVOTER_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VVOTER_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VVOTER_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VVOTER_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VVOTER_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VVOTER_COMPONENT, ICAL_NO_PROPERTY, ICAL_VAVAILABILITY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VVOTER_COMPONENT, ICAL_NO_PROPERTY, ICAL_VEVENT_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VVOTER_COMPONENT, ICAL_NO_PROPERTY, ICAL_VFREEBUSY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VVOTER_COMPONENT, ICAL_NO_PROPERTY, ICAL_VJOURNAL_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VVOTER_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VVOTER_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTODO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_VVOTER_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VVOTER_COMPONENT, ICAL_NO_PROPERTY, ICAL_XVOTE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VVOTER_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VVOTER_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VVOTER_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_VVOTER_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VVOTER_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VVOTER_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VVOTER_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VVOTER_COMPONENT, ICAL_VOTER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_VVOTER_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_ACTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_CALSCALE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_COMPLETED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_DTEND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_duration},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_DUE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_dtend},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_FREEBUSY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_METHOD_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_PERCENTCOMPLETE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_PRODID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_must_be_recurring},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_REPEAT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_TRANSP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_TRIGGER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_TZID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_TZNAME_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_TZOFFSETFROM_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_TZOFFSETTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_TZURL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_VERSION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XAVAILABLE_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_ACTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_CALSCALE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_COMPLETED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_DTEND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_DUE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_FREEBUSY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_METHOD_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_PERCENTCOMPLETE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_PRODID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_REPEAT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_TRANSP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_TRIGGER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_TZID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_TZNAME_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_TZOFFSETFROM_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_TZOFFSETTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_TZURL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_VERSION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XDAYLIGHT_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_XPATCH_COMPONENT, ICAL_NO_PROPERTY, ICAL_VPATCH_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XPATCH_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_XPATCH_COMPONENT, ICAL_NO_PROPERTY, ICAL_XPATCH_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XPATCH_COMPONENT, ICAL_PATCHDELETE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_XPATCH_COMPONENT, ICAL_PATCHPARAMETER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_XPATCH_COMPONENT, ICAL_PATCHTARGET_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_XPATCH_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_ACTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_CALSCALE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_COMPLETED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_DTEND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_DUE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_FREEBUSY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_METHOD_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_PERCENTCOMPLETE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_PRODID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_REPEAT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_TRANSP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_TRIGGER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_TZID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_TZNAME_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_TZOFFSETFROM_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_TZOFFSETTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_TZURL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_VERSION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XSTANDARD_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_XVOTE_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_XVOTE_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XVOTE_COMPONENT, ICAL_NO_PROPERTY, ICAL_VAVAILABILITY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XVOTE_COMPONENT, ICAL_NO_PROPERTY, ICAL_VEVENT_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XVOTE_COMPONENT, ICAL_NO_PROPERTY, ICAL_VFREEBUSY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XVOTE_COMPONENT, ICAL_NO_PROPERTY, ICAL_VJOURNAL_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XVOTE_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XVOTE_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTODO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XVOTE_COMPONENT, ICAL_NO_PROPERTY, ICAL_VVOTER_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_NONE, ICAL_XVOTE_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_XVOTE_COMPONENT, ICAL_POLLITEMID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_XVOTE_COMPONENT, ICAL_RESPONSE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_NONE, ICAL_XVOTE_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_ACCEPTRESPONSE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_COMPLETED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_DTEND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_duration},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_dtend},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VAVAILABILITY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VEVENT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VFREEBUSY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VJOURNAL_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, icalrestriction_must_if_tz_ref},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTODO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VVOTER_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_POLLCOMPLETION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_POLLITEMID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_POLLMODE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_POLLPROPERTIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_POLLSTATUS, ICAL_VPOLL_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VAGENDA_COMPONENT, ICAL_CALMASTER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VAGENDA_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VAGENDA_COMPONENT, ICAL_OWNER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VAGENDA_COMPONENT, ICAL_RELCALID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VAGENDA_COMPONENT, ICAL_TZID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_COLOR_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_CONCEPT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_CONFERENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_DTEND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_duration},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_dtend},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_IMAGE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_LINK_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_PARTICIPANT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VFREEBUSY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VJOURNAL_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VLOCATION_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VRESOURCE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, icalrestriction_must_if_tz_ref},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTODO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_must_be_recurring},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_REFID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_RELCALID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_validate_status_value},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_STRUCTUREDDATA_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_STYLEDDESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_TRANSP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VEVENT_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VFREEBUSY_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VFREEBUSY_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VFREEBUSY_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VFREEBUSY_COMPONENT, ICAL_DTEND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VFREEBUSY_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VFREEBUSY_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VFREEBUSY_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VFREEBUSY_COMPONENT, ICAL_FREEBUSY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_PARTICIPANT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_VEVENT_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_VJOURNAL_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_VLOCATION_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_VRESOURCE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZERO, icalrestriction_must_if_tz_ref},
    {ICAL_METHOD_PUBLISH, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTODO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VFREEBUSY_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VFREEBUSY_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VFREEBUSY_COMPONENT, ICAL_STYLEDDESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VFREEBUSY_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VFREEBUSY_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VFREEBUSY_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_COLOR_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_IMAGE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_PARTICIPANT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VEVENT_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VFREEBUSY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VLOCATION_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VRESOURCE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTODO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_must_be_recurring},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_validate_status_value},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_STRUCTUREDDATA_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_STYLEDDESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VJOURNAL_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_ACCEPTRESPONSE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_COMPLETED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_DTEND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_duration},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_dtend},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VAVAILABILITY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VEVENT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VFREEBUSY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VJOURNAL_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, icalrestriction_must_if_tz_ref},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTODO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VVOTER_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_POLLCOMPLETION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_POLLITEMID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_POLLMODE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_POLLPROPERTIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VPOLL_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_COLOR_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_CONCEPT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_CONFERENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_DUE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_no_duration},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_no_due},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_IMAGE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_LINK_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_PARTICIPANT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VEVENT_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VFREEBUSY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VJOURNAL_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VLOCATION_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VRESOURCE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_PERCENTCOMPLETE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_must_be_recurring},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_REFID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_RELCALID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_STRUCTUREDDATA_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_STYLEDDESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_PUBLISH, ICAL_VTODO_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VAGENDA_COMPONENT, ICAL_CALMASTER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VAGENDA_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VAGENDA_COMPONENT, ICAL_OWNER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VAGENDA_COMPONENT, ICAL_RELCALID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VAGENDA_COMPONENT, ICAL_TZID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_COLOR_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_CONCEPT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_CONFERENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_DTEND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_IMAGE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_LINK_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_PARTICIPANT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VFREEBUSY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VJOURNAL_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VLOCATION_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VRESOURCE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZERO, icalrestriction_must_if_tz_ref},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTODO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_must_be_recurring},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_REFID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_RELCALID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_STRUCTUREDDATA_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_STYLEDDESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_TRANSP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VEVENT_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_ACCEPTRESPONSE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_COMPLETED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_DTEND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VAVAILABILITY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VEVENT_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VFREEBUSY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VJOURNAL_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, icalrestriction_must_if_tz_ref},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTODO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VVOTER_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_POLLCOMPLETION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_POLLITEMID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_POLLMODE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_POLLPROPERTIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VPOLL_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_COLOR_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_CONCEPT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_CONFERENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_DUE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_IMAGE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_LINK_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_PARTICIPANT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VEVENT_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VFREEBUSY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VLOCATION_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VRESOURCE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_PERCENTCOMPLETE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_must_be_recurring},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_REFID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_RELCALID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_STRUCTUREDDATA_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_STYLEDDESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REFRESH, ICAL_VTODO_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VAGENDA_COMPONENT, ICAL_CALMASTER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VAGENDA_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VAGENDA_COMPONENT, ICAL_OWNER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VAGENDA_COMPONENT, ICAL_RELCALID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VAGENDA_COMPONENT, ICAL_TZID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_COLOR_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_CONCEPT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_CONFERENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_DTEND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_duration},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_dtend},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_IMAGE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_LINK_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_PARTICIPANT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VFREEBUSY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VJOURNAL_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VLOCATION_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VRESOURCE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_must_if_tz_ref},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTODO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_must_be_recurring},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_REFID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_RELCALID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_STRUCTUREDDATA_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_STYLEDDESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_TRANSP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VEVENT_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VFREEBUSY_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VFREEBUSY_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VFREEBUSY_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VFREEBUSY_COMPONENT, ICAL_DTEND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VFREEBUSY_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VFREEBUSY_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VFREEBUSY_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REPLY, ICAL_VFREEBUSY_COMPONENT, ICAL_FREEBUSY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_PARTICIPANT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REPLY, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_VEVENT_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REPLY, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_VJOURNAL_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REPLY, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_VLOCATION_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_VRESOURCE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REPLY, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTODO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REPLY, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VFREEBUSY_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VFREEBUSY_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VFREEBUSY_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REPLY, ICAL_VFREEBUSY_COMPONENT, ICAL_STYLEDDESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VFREEBUSY_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VFREEBUSY_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VFREEBUSY_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_ACCEPTRESPONSE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_COMPLETED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_DTEND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_duration},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_dtend},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VAVAILABILITY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VEVENT_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VFREEBUSY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VJOURNAL_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_must_if_tz_ref},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTODO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VVOTER_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_POLLCOMPLETION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_POLLITEMID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_POLLMODE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_POLLPROPERTIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_TRANSP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VPOLL_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_COLOR_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_CONCEPT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_CONFERENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_DUE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_no_duration},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_no_due},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_IMAGE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_LINK_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_PARTICIPANT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VEVENT_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VFREEBUSY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VLOCATION_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VRESOURCE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_PERCENTCOMPLETE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_must_be_recurring},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_REFID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_RELCALID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_STRUCTUREDDATA_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_STYLEDDESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REPLY, ICAL_VTODO_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VAGENDA_COMPONENT, ICAL_CALMASTER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VAGENDA_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VAGENDA_COMPONENT, ICAL_OWNER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VAGENDA_COMPONENT, ICAL_RELCALID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VAGENDA_COMPONENT, ICAL_TZID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_COLOR_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_CONCEPT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_CONFERENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_DTEND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_duration},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_dtend},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_IMAGE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_LINK_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_PARTICIPANT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VFREEBUSY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VJOURNAL_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VLOCATION_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VRESOURCE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, icalrestriction_must_if_tz_ref},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTODO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_must_be_recurring},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_REFID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_RELCALID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_validate_status_value},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_STRUCTUREDDATA_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_STYLEDDESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_TRANSP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VEVENT_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VFREEBUSY_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VFREEBUSY_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VFREEBUSY_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VFREEBUSY_COMPONENT, ICAL_DTEND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VFREEBUSY_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VFREEBUSY_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VFREEBUSY_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VFREEBUSY_COMPONENT, ICAL_FREEBUSY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_PARTICIPANT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_VEVENT_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_VJOURNAL_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_VLOCATION_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_VRESOURCE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTODO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VFREEBUSY_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VFREEBUSY_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VFREEBUSY_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VFREEBUSY_COMPONENT, ICAL_STYLEDDESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VFREEBUSY_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VFREEBUSY_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VFREEBUSY_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_ACCEPTRESPONSE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_COMPLETED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_DTEND_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_duration},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEEXCLUSIVE, icalrestriction_no_dtend},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VAVAILABILITY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VEVENT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VFREEBUSY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VJOURNAL_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, icalrestriction_must_if_tz_ref},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTODO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_VVOTER_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_POLLCOMPLETION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_POLLITEMID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_POLLMODE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_POLLPROPERTIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_TRANSP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VPOLL_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_ATTACH_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_ATTENDEE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_CATEGORIES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_CLASS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_COLOR_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_COMMENT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_CONCEPT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_CONFERENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_CONTACT_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_CREATED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_DESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_DTSTAMP_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_DTSTART_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_DUE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_no_duration},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_DURATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_no_due},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_EXDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_EXRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_GEO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_IMAGE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_LASTMODIFIED_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_LINK_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_LOCATION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONEPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_PARTICIPANT_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VALARM_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VEVENT_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VFREEBUSY_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VJOURNAL_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VLOCATION_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VRESOURCE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_VTIMEZONE_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_NO_PROPERTY, ICAL_X_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_ORGANIZER_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_PERCENTCOMPLETE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_PRIORITY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_RDATE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_RECURRENCEID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_must_be_recurring},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_REFID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_RELATEDTO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_RELCALID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_REQUESTSTATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZERO, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_RESOURCES_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_RRULE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_SEQUENCE_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_STATUS_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, icalrestriction_validate_status_value},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_STRUCTUREDDATA_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_STYLEDDESCRIPTION_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_SUMMARY_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_UID_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_URL_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROORONE, NULL},
    {ICAL_METHOD_REQUEST, ICAL_VTODO_COMPONENT, ICAL_X_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_ZEROPLUS, NULL},
    {ICAL_METHOD_NONE, ICAL_NO_COMPONENT, ICAL_NO_PROPERTY, ICAL_NO_COMPONENT, ICAL_RESTRICTION_NONE, NULL}
};

static const icalrestriction_record *icalrestriction_get_restriction(
    const icalrestriction_record *start,
    icalproperty_method method, icalcomponent_kind component,
    icalproperty_kind property, icalcomponent_kind subcomp)
{
    const icalrestriction_record *rec;

    if (!start) {
        start = &icalrestriction_records[0];
    }

    for (rec = start; rec && rec->restriction < ICAL_RESTRICTION_UNKNOWN && rec->restriction != ICAL_RESTRICTION_NONE; rec++) {

        if (method == rec->method &&
            (component == ICAL_ANY_COMPONENT ||
             (component == rec->component &&
              (property == ICAL_ANY_PROPERTY || property == rec->property) &&
              (subcomp == ICAL_ANY_COMPONENT || subcomp == rec->subcomponent)))) {
            return rec;
        }
    }

    return &null_restriction_record;
}
