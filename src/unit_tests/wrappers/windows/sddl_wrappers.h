/* Copyright (C) 2015-2020, Wazuh Inc.
 * Copyright (C) 2009 Trend Micro Inc.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation
 */


#ifndef SDDL_H
#define SDDL_H

#include <windows.h>

WINBOOL wrap_ConvertSidToStringSid(PSID Sid,LPSTR *StringSid);

#endif
