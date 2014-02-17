#!/usr/bin/python

# A small script, which outputs the download statistics for Ubuntu PPA (unfortunately, there is no web interface to see it).

import sys
from launchpadlib.launchpad import Launchpad

PPAOWNER = 'purple-vk-plugin'
PPA = 'dev'
SERIES = ['precise', 'saucy']
ARCH = ['i386', 'amd64']

def distro_arch_series(series, arch):
    return 'https://api.launchpad.net/devel/ubuntu/{0}/{1}'.format(series, arch)

CACHEDIR = "~/.cache/launchpadlib/"
lp = Launchpad.login_anonymously('ppastats', 'edge', CACHEDIR, version='devel')
ppa = lp.people[PPAOWNER].getPPAByName(name=PPA)

for series in SERIES:
    for arch in ARCH:
        distro = distro_arch_series(series, arch)
        for ar in ppa.getPublishedBinaries(status='Published', distro_arch_series=distro):
            count = ar.getDownloadCount()
            if count > 0:
                print('{0}: {1} {2}: {3}'.format(ar.binary_package_name, ar.binary_package_version, arch, count))

            totals = ar.getDailyDownloadTotals()
            lastdays = sorted(totals)[-7:]
            for day in lastdays:
                print('{0}: {1}'.format(day, totals[day]))
