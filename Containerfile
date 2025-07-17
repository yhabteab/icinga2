# Icinga 2 Docker image | (c) 2025 Icinga GmbH | GPLv2+

FROM debian:bookworm-slim AS build-base
SHELL ["/bin/bash", "-o", "pipefail", "-c"]

# Install all the necessary build dependencies for building Icinga 2 and the plugins.
#
# This stage includes the build dependencies for the plugins as well, so that they can share the same base
# image, since Docker builds common stages only once [^1] even if they are used in multiple build stages.
# This eliminates the need to have a separate base image for the plugins, that basically has kind of the
# same dependencies as the Icinga 2 build stage (ok, not exactly the same, but some of them are shared).
#
# [^1]: https://docs.docker.com/build/building/best-practices/#create-reusable-stages
RUN apt-get update && \
    apt-get install -y --no-install-{recommends,suggests} \
        autoconf \
        automake \
        bison \
        ccache \
        cmake \
        flex \
        g++ \
        git \
        libboost{,-{context,coroutine,date-time,filesystem,iostreams,program-options,regex,system,test,thread}}1.74-dev \
        libedit-dev \
        libmariadb-dev \
        libpq-dev \
        libssl-dev \
        libsystemd-dev \
        make && \
    rm -rf /var/lib/apt/lists/*

# Set the default working directory for subsequent commands of the next stages.
WORKDIR /icinga2-build

FROM build-base AS build-plugins
SHELL ["/bin/bash", "-o", "pipefail", "-c"]

# Install all the plugins that are not included in the monitoring-plugins package.
ADD https://github.com/lausser/check_mssql_health.git#747af4c3c261790341da164b58d84db9c7fa5480 /check_mssql_health
ADD https://github.com/lausser/check_nwc_health.git#a5295475c9bbd6df9fe7432347f7c5aba16b49df /check_nwc_health
ADD https://github.com/bucardo/check_postgres.git#58de936fdfe4073413340cbd9061aa69099f1680 /check_postgres
ADD https://github.com/matteocorti/check_ssl_cert.git#341b5813108fb2367ada81e866da989ea4fb29e7 /check_ssl_cert

WORKDIR /check_mssql_health
RUN mkdir bin && \
    autoconf && \
    autoreconf && \
    ./configure "--build=$(uname -m)-unknown-linux-gnu" --libexecdir=/usr/lib/nagios/plugins && \
    make && \
    make install DESTDIR="$(pwd)/bin"

WORKDIR /check_nwc_health
RUN mkdir bin && \
    autoreconf && \
    ./configure "--build=$(uname -m)-unknown-linux-gnu" --libexecdir=/usr/lib/nagios/plugins && \
    make && \
    make install DESTDIR="$(pwd)/bin"

WORKDIR /check_postgres
RUN mkdir bin && \
    perl Makefile.PL INSTALLSITESCRIPT=/usr/lib/nagios/plugins && \
    make && \
    make install DESTDIR="$(pwd)/bin" && \
    # The is is necessary because of this build error: cannot copy to non-directory: /var/lib/docker/.../merged/usr/local/man
    rm -rf bin/usr/local/man

FROM build-base AS build-icinga2
SHELL ["/bin/bash", "-o", "pipefail", "-c"]

# To access the automated build arguments in the Dockerfile originated from the Docker BuildKit [^1],
# we need to declare them here as build arguments. This is necessary because we want to use unique IDs
# for the mount cache below for each platform to avoid conflicts between multi arch builds. Otherwise,
# the build targets will invalidate the cache one another, leading to strange build errors.
#
# https://docs.docker.com/reference/dockerfile/#automatic-platform-args-in-the-global-scope
ARG TARGETPLATFORM

# Create the directory where the final Icinga 2 files will be installed.
#
# This directory will be used as the destination for the `make install` command below and will be
# copied to the final image. Other than that, this directory will not be used for anything else.
RUN mkdir /icinga2-install

# Mount the source code as a bind mount instead of copying it, so that we can use the cache effectively.
# Additionally, add the ccache and CMake build directories as cache mounts to speed up rebuilds.
RUN --mount=type=bind,source=.,target=/icinga2,readonly \
    --mount=type=cache,id=ccache-${TARGETPLATFORM},target=/root/.ccache \
    --mount=type=cache,id=icinga2-build-${TARGETPLATFORM},target=/icinga2-build \
    PATH="/usr/lib/ccache:$PATH" \
    cmake -S /icinga2 -B /icinga2-build \
        -DCMAKE_BUILD_TYPE=ReleaseWithDebInfo \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_INSTALL_SYSCONFDIR=/etc \
        -DCMAKE_INSTALL_LOCALSTATEDIR=/var \
        -DICINGA2_SYSCONFIGFILE=/etc/sysconfig/icinga2 \
        -DICINGA2_RUNDIR=/run \
        -DUSE_SYSTEMD=ON \
        -DICINGA2_WITH_{COMPAT,LIVESTATUS}=OFF && \
    make -C /icinga2-build \
        -j$(nproc) && \
    CTEST_OUTPUT_ON_FAILURE=1 make -C /icinga2-build test && \
    make install DESTDIR=/icinga2-install

RUN rm -rf /icinga2-install/etc/icinga2/features-enabled/mainlog.conf \
    /icinga2-install/usr/share/doc/icinga2/markdown && \
    strip -g /icinga2-install/usr/lib/*/icinga2/sbin/icinga2 && \
    strip -g /icinga2-install/usr/lib/nagios/plugins/check_nscp_api

# Prepare the final image with the necessary configuration files and runtime dependencies.
FROM debian:bookworm-slim AS icinga2
SHELL ["/bin/bash", "-o", "pipefail", "-c"]

# Install the necessary runtime dependencies for the Icinga 2 binary and the monitoring-plugins.
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive && \
    apt-get install -y --no-install-{recommends,suggests} \
        bc \
        ca-certificates \
        curl \
        dumb-init \
        file \
        libboost-{context,coroutine,date-time,filesystem,iostreams,program-options,regex,system,thread}1.74.0 \
        libcap2-bin \
        libedit2 \
        libldap-common \
        libmariadb3 \
        libmoosex-role-timer-perl \
        libpq5 \
        libssl3 \
        libsystemd0 \
        mailutils \
        monitoring-plugins \
        msmtp{,-mta} \
        openssh-client \
        openssl && \
    # Official Debian images automatically run `apt-get clean` after every install, so we don't need to do it here.
    rm -rf /var/lib/apt/lists/*

# Create the icinga user and group with a specific UID as recommended by Docker best practices.
# The user has a home directory at /var/lib/icinga2, and if configured, that directory will also
# be used to store the ".msmtprc" file created by the entrypoint script.
RUN adduser \
    --system \
    --group \
    --home /var/lib/icinga2 \
    --disabled-login \
    --allow-bad-names \
    --no-create-home \
    --uid 5665 icinga

COPY --from=build-plugins /check_mssql_health/bin/ /
COPY --from=build-plugins /check_nwc_health/bin/ /
COPY --from=build-plugins /check_postgres/bin/ /
COPY --from=build-plugins /check_ssl_cert/check_ssl_cert /usr/lib/nagios/plugins/check_ssl_cert

COPY --from=build-icinga2 /icinga2-install/ /

# Create the /data directory where all the configuration files will be stored.
# This directory will be used as a volume to store the configuration files and other data.
RUN mkdir /data

# Move all the configuration files to the /data directory instead of to some other intermediate location
# like how it was done in the past (e.g. /data-init/{var/lib/icinga2,etc/icinga2} etc.). Doing so will allow
# the user to easily mount files/directories to /data without any issues.
#
# After copying, the original directories will be removed and symlinks will be created from /data to the
# original locations instead.
RUN for dir in /etc/icinga2 /var/cache/icinga2 /var/lib/icinga2 /var/log/icinga2 /var/spool/icinga2; do \
    target_dir="/data$dir"; \
    mkdir -p "$target_dir"; \
    cp -rv "$dir/"* "$target_dir"; \
    rm -rf "$dir"; \
    ln -vs "$target_dir" "$dir"; \
done

# Fix the permissions of the /data directory and its contents.
RUN chown -R icinga:icinga /data

# The /run/icinga2 directory is typically created, well, at runtime by the init system in a normal installation.
# However, since we basically copied all the directories installed by "make install", which includes the /run/icinga2
# directory, we need to make sure it has the correct owner. Also, we didn't move this directory to the /data directory
# like we did with the others above, because its contents are ephemeral and not meant to be persisted.
RUN chown -R icinga:icinga /run/icinga2

# Well, since the /data directory is intended to be used as a volume, we should also declare it as such.
# This will allow users to mount their own directories or even specific files to the /data directory
# without any issues. We've already filled the /data directory with the necessary configuration files,
# so users can simply mount their own files or directories if they want to override the default ones and
# they will be able to do so without any issues.
VOLUME ["/data"]

COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
RUN chmod +x /usr/local/bin/docker-entrypoint.sh
ENTRYPOINT ["/usr/bin/dumb-init", "-c", "--", "/usr/local/bin/docker-entrypoint.sh"]

EXPOSE 5665
USER icinga

CMD ["icinga2", "daemon"]
