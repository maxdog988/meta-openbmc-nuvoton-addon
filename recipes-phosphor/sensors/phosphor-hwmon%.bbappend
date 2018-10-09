FILESEXTRAPATHS_prepend := "${THISDIR}/${PN}:"

SRCREV = "af6227a3eb3fc918425d7588e001a75bd348232d"
SRC_URI += "file://0001-generate_sel_event.patch"

NAMES = " \
        i2c-bus@f0082000/tmp100@48 \
        i2c-bus@f0081000/lm75@48 \
        "
ITEMSFMT = "apb/{0}.conf"

ITEMS += "${@compose_list(d, 'ITEMSFMT', 'NAMES')}"

ENVS = "obmc/hwmon/{0}"
SYSTEMD_ENVIRONMENT_FILE_${PN} += "${@compose_list(d, 'ENVS', 'ITEMS')}"

# Fan sensors
FITEMS = "fan@0.conf"
FENVS = "obmc/hwmon/apb/{0}"
SYSTEMD_ENVIRONMENT_FILE_${PN} += "${@compose_list(d, 'FENVS', 'FITEMS')}"
