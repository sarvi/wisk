#!/bin/bash
set -x

echo "Executing code under Test"
cd /witspace/sarvi/space_sarvi_wiskxr1


# rm -f ./kvm-calvados-stage/ncs5500-sysadmin-sdk/rpm/x86_64/*.rpm
# echo "SARVI: 1"
# if [ "1.0" = "1.0" ]; then \
#       for spf in ./kvm-calvados-stage/ncs5500-sysadmin-spec/ncs5500-sysadmin-*.host.spec; do \
#         log=`egrep '^(Name|VMTYPE): ' $spf | awk '{print $NF}'` && \
#         log=`echo $log | sed -e 's/ /-/'` && \
#         /auto/thirdparty-sdk/host-x86_64/lib/rpm-5.1.9/rpmbuild  -bb \
#         --target x86_64 \
#         --macros=/witspace/sarvi/space_sarvi_wiskxr1/calvados/hostos_pkg/boot/scripts/Macros \
#         --define='__os_install_post %nil' --quiet \
#         --define='_tmppath /witspace/sarvi/space_sarvi_wiskxr1/tmp' $spf >/witspace/sarvi/space_sarvi_wiskxr1/tmp/$log.log 2>&1; \
#         mv $spf $spf.moved; \
#       done; \
#       for spf in ./kvm-calvados-stage/ncs5500-sysadmin-spec/ncs5500-sysadmin-*.spec; do \
#         log=`egrep '^(Name|VMTYPE): ' $spf | awk '{print $NF}'` && \
#         log=`echo $log | sed -e 's/ /-/'` && \
#         /auto/thirdparty-sdk/host-x86_64/lib/rpm-5.1.9/rpmbuild  -bb \
#         --target x86_64 \
#         --macros=/witspace/sarvi/space_sarvi_wiskxr1/calvados/hostos_pkg/boot/scripts/Macros \
#         --define='__os_install_post %nil' --quiet \
#         --define='_tmppath /witspace/sarvi/space_sarvi_wiskxr1/tmp' $spf > \
#         /witspace/sarvi/space_sarvi_wiskxr1/tmp/$log.log 2>&1 & \
#         pids="$pids $!"; \
#       done; \
#       bg_failed=0; \
#       for n in $pids; \
#       do \
#         if ! wait $n; then \
#           bg_failed=$((++bg_failed)); \
#         fi; \
#       done; \
#       for f in ./kvm-calvados-stage/ncs5500-sysadmin-spec/ncs5500-sysadmin-*.spec.moved; \
#       do \
#         orig=`echo $f | sed -e 's#\.moved##'`; \
#         mv $f $orig; \
#       done; \
#       echo "Exiting with Error $bg_failed" ; \
#       exit $bg_failed; \
#     else \
#       for spf in ./kvm-calvados-stage/ncs5500-sysadmin-spec/ncs5500-sysadmin-*.spec; \
#       do \
#         log=`head -30 $spf | egrep '^(Name|VMTYPE): ' | awk '{print $NF}'`; \
#         log=`echo $log | sed -e 's/ /-/g'`; \
#         /auto/thirdparty-sdk/host-x86_64/lib/rpm-5.1.9/rpmbuild  -bb \
#         --target x86_64 \
#         --define='__os_install_post %nil' --quiet \
#         --define='_tmppath /witspace/sarvi/space_sarvi_wiskxr1/tmp' $spf \
#         > /witspace/sarvi/space_sarvi_wiskxr1/tmp/ncs5500-sysadmin-rpmbuild.log 2>&1; \
#       done; \
#     fi
# echo "SARVI: 2"
/witspace/sarvi/space_sarvi_wiskxr1/tools/misc/SignRpm -plat ncs5500-sysadmin -arch x86_64 \
      ./kvm-calvados-stage/ncs5500-sysadmin-sdk/rpm/x86_64/*.rpm \
      > /witspace/sarvi/space_sarvi_wiskxr1/ncs5500-sysadmin-rpmsign.log 2>&1
touch ./kvm-calvados-stage/x86_64/ncs5500-sysadmin-rpm.sentinel
mkdir -p  /witspace/sarvi/space_sarvi_wiskxr1/./kvm-calvados-stage/ncs5500-sysadmin-sdk/x86_64
# rsync exported SDK header files to calvados sdk component dir.
from_dir="/witspace/sarvi/space_sarvi_wiskxr1/kvm-calvados-stage/x86_64/opt/cisco/calvados/usr/include/" && \
    to_dir="/witspace/sarvi/space_sarvi_wiskxr1/calvados/sdk/x86_64/opt/cisco/calvados/usr/include/" && \
    mkdir -p $to_dir && \
    /bin/rsync -crlpgoH --delete $from_dir $to_dir
# Copy exported SDK libraries to calvados sdk component dir.
cd /witspace/sarvi/space_sarvi_wiskxr1/kvm-calvados-stage/sdk/x86_64 && \
      tar cf - . | (cd /witspace/sarvi/space_sarvi_wiskxr1/calvados/sdk/x86_64; tar xfB -)
# Populate manifest.txt and version_labels.txt to /witspace/sarvi/space_sarvi_wiskxr1/calvados/sdk/x86_64
(cd /witspace/sarvi/space_sarvi_wiskxr1/calvados/sdk/x86_64 && \
    printf "%-13s%s\n" "Built By" ": sarvi" > manifest.txt && \
    printf "%-13s%s\n" "Built On" ": `date`" >> manifest.txt && \
    printf "%-13s%s\n" "Build Host" ": `hostname`" >> manifest.txt && \
    acme desc -workspace -s | head -5 | \
    grep -v "Devline Ver" >> manifest.txt && \
    /witspace/sarvi/space_sarvi_wiskxr1/tools/misc/ws_efr >> manifest.txt && \
    sed -i -e "s|\(Workspace\s*:\s*\).*|\1/witspace/sarvi/space_sarvi_wiskxr1|" manifest.txt && \
    cp manifest.txt /witspace/sarvi/space_sarvi_wiskxr1/calvados/release/x86_64-manifest.txt && \
    cp /witspace/sarvi/space_sarvi_wiskxr1/package/version_labels/src/version_labels.txt  . )
# Populate rpm tree to /witspace/sarvi/space_sarvi_wiskxr1/calvados/sdk/x86_64
cd /witspace/sarvi/space_sarvi_wiskxr1/./kvm-calvados-stage/ncs5500-sysadmin-sdk/rpm && \
    tar cf - x86_64 | (cd /witspace/sarvi/space_sarvi_wiskxr1/calvados/sdk/x86_64/.. ; tar xfB -)
# Populate ctrace_dec tree to /witspace/sarvi/space_sarvi_wiskxr1/calvados/sdk/x86_64
[ -d /witspace/sarvi/space_sarvi_wiskxr1/ctrace_dec/x86_64 ] && \
    cd /witspace/sarvi/space_sarvi_wiskxr1/ctrace_dec/x86_64 && \
    tar cf - . | (cd /witspace/sarvi/space_sarvi_wiskxr1/calvados/sdk/x86_64 ; tar xfB -)
# Next populate calvados tools tree to /witspace/sarvi/space_sarvi_wiskxr1/calvados/sdk/x86_64
cd /witspace/sarvi/space_sarvi_wiskxr1 && \
    tar cf - tools/capi tools/capi-* tools/ctrace | (cd /witspace/sarvi/space_sarvi_wiskxr1/calvados/sdk/x86_64/.. ; tar xfB -) # && \
    # rm -f -r /witspace/sarvi/space_sarvi_wiskxr1/./kvm-calvados-stage/ncs5500-sysadmin-sdk/x86_64


echo "Result: $?"
