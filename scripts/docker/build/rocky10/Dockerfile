# Auto generated for rocky10
# from scripts/docker/m4/docker.rpm.m4
#
# Rebuild this file with `make docker.rocky10.regen`
#
ARG from=rockylinux/rockylinux:10
FROM ${from} AS build

RUN rpmkeys --import /etc/pki/rpm-gpg/RPM-GPG-KEY-Rocky-10

#
#  Install build tools
#
RUN dnf update -y
RUN dnf install -y rpmdevtools openssl epel-release git procps dnf-utils \
	rsync dnf-plugins-core

RUN dnf config-manager --set-enabled crb

#
#  Set up NetworkRADIUS extras repository
#
RUN echo $'[networkradius-extras]\n\
name=NetworkRADIUS-extras-$releasever\n\
baseurl=http://packages.networkradius.com/extras/rocky/$releasever/\n\
enabled=1\n\
gpgcheck=1\n\
gpgkey=https://packages.networkradius.com/pgp/packages@networkradius.com'\
> /etc/yum.repos.d/networkradius-extras.repo
RUN rpm --import https://packages.networkradius.com/pgp/packages@networkradius.com

#
#  Create build directory
#
RUN mkdir -p /usr/local/src/repositories/freeradius-server
WORKDIR /usr/local/src/repositories/freeradius-server/

#
#  Copy the FreeRADIUS directory in
#
COPY . .

#
#  Clean up tree - we want to build from the latest commit, not from
#  any cruft left around on the local system
#
RUN git clean -fdxx \
 && git reset --hard HEAD

#
#  Install build dependencies
#
#  Run twice, it doesn't always get everything with one invocation
RUN [ -e redhat/freeradius.spec ] && \
	dnf builddep -y redhat/freeradius.spec && \
	dnf builddep -y redhat/freeradius.spec

#
#  Create RPM build environment
#
ENV BUILDDIR=/root/rpmbuild
RUN rpmdev-setuptree

RUN ./configure
RUN make pkg_version > /VERSION
RUN cat /VERSION
RUN make freeradius-server-$(cat /VERSION).tar.bz2
RUN cp freeradius-server-$(cat /VERSION).tar.bz2 $BUILDDIR/SOURCES/
RUN cp -r redhat/* $BUILDDIR/SOURCES/
RUN make dist-check-rpm
RUN cp -r redhat/freeradius.spec $BUILDDIR/SPECS/
WORKDIR $BUILDDIR

#
#  Build the server
#
ENV QA_RPATHS=0x0003
RUN rpmbuild -bb --define "_release $(cat /VERSION)" "$BUILDDIR/SPECS/freeradius.spec"

RUN mkdir /root/rpms
RUN mv $BUILDDIR/RPMS/*/*.rpm /root/rpms/

#
#  Clean environment and run the server
#
FROM ${from}

COPY --from=build /root/rpms /tmp/

#
#  Set up NetworkRADIUS extras repository
#
RUN echo $'[networkradius-extras]\n\
name=NetworkRADIUS-extras-$releasever\n\
baseurl=http://packages.networkradius.com/extras/rocky/$releasever/\n\
enabled=1\n\
gpgcheck=1\n\
gpgkey=https://packages.networkradius.com/pgp/packages@networkradius.com'\
> /etc/yum.repos.d/networkradius-extras.repo
RUN rpm --import https://packages.networkradius.com/pgp/packages@networkradius.com

#
#  Other requirements
#

#  Needed for mysql-libs
RUN dnf install -y dnf-utils
RUN dnf config-manager --enable crb

#  EPEL repository for freetds and hiredis
RUN dnf install -y https://dl.fedoraproject.org/pub/epel/epel-release-latest-10.noarch.rpm \
    && dnf install -y dnf-utils \
    && dnf config-manager --enable epel-testing

ARG radiusd_uid=95
ARG radiusd_gid=95

RUN groupadd -g ${radiusd_gid} -r radiusd \
    && useradd -u ${radiusd_uid} -g radiusd -r -M -d /home/radiusd -s /sbin/nologin radiusd \
    && dnf install -y /tmp/*.rpm

WORKDIR /
COPY scripts/docker/etc/docker-entrypoint.sh.rpm docker-entrypoint.sh
RUN chmod +x docker-entrypoint.sh

EXPOSE 1812/udp 1813/udp
ENTRYPOINT ["/docker-entrypoint.sh"]
CMD ["radiusd"]
