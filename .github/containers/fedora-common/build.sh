#! /bin/bash

set -ex

docker_image="$1"
rpmbuild="$2"

PLUGIN_NAME=$(awk '/^project\(/{print gensub(/project\(([^ ()]*).*/, "\\1", 1, $0)}' CMakeLists.txt)
VERSION=$(git describe --tag --always | sed -e 's/-\(alpha\|beta\|rc\)/~\1/g' -e 's/-/./g')

rm -rf $rpmbuild
mkdir -p $rpmbuild/{BUILD,BUILDROOT,SRPMS,SOURCES,SPECS,RPMS}
rpmbuild="$(cd $rpmbuild && pwd -P)"
chmod a+w $rpmbuild/{BUILD,BUILDROOT,SRPMS,RPMS}
test -x /usr/sbin/selinuxenabled && /usr/sbin/selinuxenabled && chcon -Rt container_file_t $rpmbuild

# Prepare files
sed \
	-e "s/@PLUGIN_NAME@/$PLUGIN_NAME/g" \
	-e "s/@VERSION@/$VERSION/g" \
	< ci/plugin.spec \
	> $rpmbuild/SPECS/$PLUGIN_NAME.spec

git archive --format=tar --prefix=$PLUGIN_NAME-$VERSION/ HEAD | bzip2 > $rpmbuild/SOURCES/$PLUGIN_NAME-$VERSION.tar.bz2

# FIXME: Remove a workaround QA_RPATHS, which avoid failure caused by the so file containing standard runpath.
docker run -v $rpmbuild:/home/rpm/rpmbuild $docker_image bash -c "
sudo dnf builddep -y ~/rpmbuild/SPECS/$PLUGIN_NAME.spec &&
sudo chown 0.0 ~/rpmbuild/SOURCES/* &&
sudo chown 0.0 ~/rpmbuild/SPECS/* &&
QA_RPATHS=0x0001 rpmbuild -ba ~/rpmbuild/SPECS/$PLUGIN_NAME.spec
"
