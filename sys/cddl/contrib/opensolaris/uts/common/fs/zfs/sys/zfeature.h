/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2013 by Delphix. All rights reserved.
 */

#ifndef _SYS_ZFEATURE_H
#define	_SYS_ZFEATURE_H

#include <sys/nvpair.h>
#include "zfeature_common.h"

#ifdef	__cplusplus
extern "C" {
#endif

struct spa;
struct dmu_tx;
struct objset;

extern boolean_t feature_is_supported(struct objset *os, uint64_t obj,
    uint64_t desc_obj, nvlist_t *unsup_feat, nvlist_t *enabled_feat);

extern void spa_feature_create_zap_objects(struct spa *, struct dmu_tx *);
extern void spa_feature_enable(struct spa *, zfeature_info_t *,
    struct dmu_tx *);
extern void spa_feature_incr(struct spa *, zfeature_info_t *, struct dmu_tx *);
extern void spa_feature_decr(struct spa *, zfeature_info_t *, struct dmu_tx *);
extern boolean_t spa_feature_is_enabled(struct spa *, zfeature_info_t *);
extern boolean_t spa_feature_is_active(struct spa *, zfeature_info_t *);
extern int spa_feature_get_refcount(struct spa *, zfeature_info_t *);

#ifdef	__cplusplus
}
#endif

#endif /* _SYS_ZFEATURE_H */
