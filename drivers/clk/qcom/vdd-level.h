#ifndef __DRIVERS_CLK_QCOM_VDD_LEVEL_H
#define __DRIVERS_CLK_QCOM_VDD_LEVEL_H

#include <dt-bindings/power/qcom-rpmhpd.h>

enum vdd_levels {
        VDD_NONE,
        VDD_MIN,                /* MIN SVS */
        VDD_LOWER,              /* SVS2 */
        VDD_LOW,                /* SVS */
        VDD_LOW_L1,             /* SVSL1 */
        VDD_NOMINAL,            /* NOM */
        VDD_HIGH,               /* TURBO */
        VDD_NUM,
};

static int vdd_corner[] = {
        RPMH_REGULATOR_LEVEL_OFF,               /* VDD_NONE */
        RPMH_REGULATOR_LEVEL_MIN_SVS,           /* VDD_MIN */
        RPMH_REGULATOR_LEVEL_LOW_SVS,           /* VDD_LOWER */
        RPMH_REGULATOR_LEVEL_SVS,               /* VDD_LOW */
        RPMH_REGULATOR_LEVEL_SVS_L1,            /* VDD_LOW_L1 */
        RPMH_REGULATOR_LEVEL_NOM,               /* VDD_NOMINAL */
        RPMH_REGULATOR_LEVEL_TURBO,             /* VDD_HIGH */
};

#endif

