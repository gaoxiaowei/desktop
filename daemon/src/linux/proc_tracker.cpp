// Copyright (c) 2020 Private Internet Access, Inc.
//
// This file is part of the Private Internet Access Desktop Client.
//
// The Private Internet Access Desktop Client is free software: you can
// redistribute it and/or modify it under the terms of the GNU General Public
// License as published by the Free Software Foundation, either version 3 of
// the License, or (at your option) any later version.
//
// The Private Internet Access Desktop Client is distributed in the hope that
// it will be useful, but WITHOUT ANY WARRANTY; without even the implied
// warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with the Private Internet Access Desktop Client.  If not, see
// <https://www.gnu.org/licenses/>.


#include <linux/netlink.h>
#include <linux/cn_proc.h>
#include <linux/connector.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <QRegularExpression>
#include <QSocketNotifier>
#include <QRegularExpression>
#include <QPointer>
#include <QFile>
#include <QDir>

#include "daemon.h"
#include "path.h"
#include "posix/posix_firewall_iptables.h"
#include "proc_tracker.h"

namespace
{
    RegisterMetaType<QVector<QString>> qStringVector;
    RegisterMetaType<OriginalNetworkScan> qNetScan;
    RegisterMetaType<FirewallParams> qFirewallParams;
}

QSet<pid_t> ProcFs::filterPids(const std::function<bool(pid_t)> &filterFunc)
{
    QDir procDir{"/proc"};
    procDir.setFilter(QDir::Dirs);
    procDir.setNameFilters({"[1-9]*"});

    QSet<pid_t> filteredPids;
    for(const auto &entry : procDir.entryList())
    {
        pid_t pid = entry.toInt();
        if(filterFunc(pid))
            filteredPids.insert(pid);
    }

    return filteredPids;
}

QSet<pid_t> ProcFs::pidsForPath(const QString &path)
{
    return filterPids([&](pid_t pid) { return pathForPid(pid) == path; });
}

QSet<pid_t> ProcFs::childPidsOf(pid_t parentPid)
{
    return filterPids([&](pid_t pid) { return isChildOf(parentPid, pid); });
}

QString ProcFs::pathForPid(pid_t pid)
{
    QString link = QStringLiteral("/proc/%1/exe").arg(pid);
    return QFile::symLinkTarget(link);
}

bool ProcFs::isChildOf(pid_t parentPid, pid_t pid)
{
    static const QRegularExpression parentPidRegex{ QStringLiteral("PPid:\\s+([0-9]+)") };

    QFile statusFile{QStringLiteral("/proc/%1/status").arg(pid)};
    if(!statusFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    auto match = parentPidRegex.match(statusFile.readAll());
    if(match.hasMatch())
    {
        auto foundParentPid = match.captured(1).toInt();
        return foundParentPid == parentPid;
    }

    return false;
}

// Explicitly specify struct alignment
typedef struct __attribute__((aligned(NLMSG_ALIGNTO)))
{
    nlmsghdr header;

    // Insert no padding as we want the members contiguous
    struct __attribute__((__packed__))
    {
        cn_msg body;
        proc_cn_mcast_op subscription_type;
    };
} NetlinkRequest;

typedef struct __attribute__((aligned(NLMSG_ALIGNTO)))
{
    nlmsghdr header;

    struct __attribute__((__packed__))
    {
        cn_msg body;
        proc_event event;
    };
} NetlinkResponse;

void ProcTracker::showError(QString funcName)
{
    qWarning() << QStringLiteral("%1 Error (code: %2) %3").arg(funcName).arg(errno).arg(qPrintable(qt_error_string(errno)));
}

void ProcTracker::writePidToCGroup(pid_t pid, const QString &cGroupPath)
{
    QFile cGroupFile{cGroupPath};

    if(!cGroupFile.open(QFile::WriteOnly))
    {
        qWarning() << "Cannot open" << cGroupPath << "for writing!" << cGroupFile.errorString();
        return;
    }

    if(cGroupFile.write(QByteArray::number(pid)) < 0)
        qWarning() << "Could not write to" << cGroupPath << cGroupFile.errorString();
}

void ProcTracker::addPidToCgroup(pid_t pid, const Path &cGroupPath)
{
    writePidToCGroup(pid, cGroupPath);
    // Add child processes (NOTE: we also recurse through child processes of child processes)
    addChildPidsToCgroup(pid, cGroupPath);
}

void ProcTracker::addChildPidsToCgroup(pid_t parentPid, const Path &cGroupPath)
{
    for(pid_t pid : ProcFs::childPidsOf(parentPid))
    {
        qInfo() << "Adding child pid" << pid;
        addPidToCgroup(pid, cGroupPath);
    }
}

void ProcTracker::removeChildPidsFromCgroup(pid_t parentPid, const Path &cGroupPath)
{
    for(pid_t pid : ProcFs::childPidsOf(parentPid))
    {
        qInfo() << "Removing child pid" << pid << cGroupPath;
        removePidFromCgroup(pid, cGroupPath);
    }
}

void ProcTracker::removePidFromCgroup(pid_t pid, const Path &cGroupPath)
{
    // We remove a PID from a cgroup by adding it to its parent cgroup
    writePidToCGroup(pid, cGroupPath);
    // Remove child processes (NOTE: we also recurse through child processes of child processes)
    removeChildPidsFromCgroup(pid, cGroupPath);
}

void ProcTracker::updateMasquerade(QString interfaceName)
{
    if(interfaceName.isEmpty())
    {
        qInfo() << "Removing masquerade rule, not connected";
        IpTablesFirewall::replaceAnchor(
            IpTablesFirewall::Both,
            QStringLiteral("100.transIp"),
            {},
            IpTablesFirewall::kNatTable
        );
    }
    else
    {
        qInfo() << "Updating the masquerade rule for new interface name" << interfaceName;
        IpTablesFirewall::replaceAnchor(
            IpTablesFirewall::Both,
            QStringLiteral("100.transIp"),
            {
                QStringLiteral("-o %1 -j MASQUERADE").arg(interfaceName),
                QStringLiteral("-o tun+ -j MASQUERADE")
            },
            IpTablesFirewall::kNatTable
        );
    }
}

void ProcTracker::updateRoutes(QString gatewayIp, QString interfaceName, QString tunnelDeviceName, QString tunnelDeviceRemoteAddress)
{
    const QString routingTableName = IpTablesFirewall::kRtableName;
    const QString vpnOnlyRoutingTableName = IpTablesFirewall::kVpnOnlyRtableName;

    qInfo() << "Updating the default route in"
        << routingTableName
        << "for"
        << gatewayIp
        << "and"
        << interfaceName
        << "and"
        << "tunnel interface"
        << tunnelDeviceName;

    // The bypass route can be left as-is if the configuration is not known,
    // even though the route may be out of date - we don't put any processes in
    // this cgroup when not connected.
    if(gatewayIp.isEmpty() || interfaceName.isEmpty())
    {
        qInfo() << "Not updating bypass route - configuration not known - address:"
            << gatewayIp << "- interface:" << interfaceName;
    }
    else
    {
        auto cmd = QStringLiteral("ip route replace default via %1 dev %2 table %3").arg(gatewayIp, interfaceName, routingTableName);
        qInfo() << "Executing:" << cmd;
        ::shellExecute(cmd);
    }

    // The VPN-only route can be left as-is if we're not connected, VPN-only
    // processes are expected to lose connectivity in that case.
    if(tunnelDeviceRemoteAddress.isEmpty() || tunnelDeviceName.isEmpty())
    {
        qWarning() << "Tunnel configuration not known yet, can't configure VPN-only route yet - address:"
            << tunnelDeviceRemoteAddress << "- interface:" << tunnelDeviceName;
    }
    else
    {
        auto cmd = QStringLiteral("ip route replace default via %1 dev %2 table %3").arg(tunnelDeviceRemoteAddress, tunnelDeviceName, vpnOnlyRoutingTableName);
        qInfo() << "Executing:" << cmd;
        ::shellExecute(cmd);
    }

    ::shellExecute(QStringLiteral("ip route flush cache"));
}

void ProcTracker::updateNetwork(const FirewallParams &params, QString tunnelDeviceName,
                                QString tunnelDeviceLocalAddress, QString tunnelDeviceRemoteAddress)
{
    qInfo() << "previous gateway IP is" << _previousNetScan.gatewayIp();
    qInfo() << "updated gateway IP is" << params.splitTunnelNetScan.gatewayIp();
    qInfo() << "tunnel device is" << tunnelDeviceName;

    if(_previousNetScan.interfaceName() != params.splitTunnelNetScan.interfaceName())
        updateMasquerade(params.splitTunnelNetScan.interfaceName());

    // Ensure that packets with the source IP of the physical interface go out the physical interface
    if(_previousNetScan.ipAddress() != params.splitTunnelNetScan.ipAddress())
    {
        // Remove the old one (if it exists) before adding a new one
        removeRoutingPolicyForSourceIp(_previousNetScan.ipAddress(), IpTablesFirewall::kRtableName);
        addRoutingPolicyForSourceIp(params.splitTunnelNetScan.ipAddress(), IpTablesFirewall::kRtableName);
    }

    // Ensure that packets with source IP of the tunnel go out the tunnel interface
    if(_previousTunnelDeviceLocalAddress !=  tunnelDeviceLocalAddress)
    {
        // Remove the old one (if it exists) before adding a new one
        removeRoutingPolicyForSourceIp(_previousTunnelDeviceLocalAddress, IpTablesFirewall::kVpnOnlyRtableName);
        addRoutingPolicyForSourceIp(tunnelDeviceLocalAddress, IpTablesFirewall::kVpnOnlyRtableName);
    }

    // always update the routes - as we use 'route replace' so we don't have to worry about adding the same route multiple times
    updateRoutes(params.splitTunnelNetScan.gatewayIp(), params.splitTunnelNetScan.interfaceName(), tunnelDeviceName, tunnelDeviceRemoteAddress);

    // If we just got a valid network scan (we're connecting) or we lost it
    // (we're disconnected), the subsequent call to updateApps() will add/remove
    // all excluded apps (which are only tracked when we have a network scan).
    _previousNetScan = params.splitTunnelNetScan;
    _previousTunnelDeviceLocalAddress = tunnelDeviceLocalAddress;
}

void ProcTracker::initiateConnection(const FirewallParams &params,
                                     QString tunnelDeviceName, QString tunnelDeviceLocalAddress, QString tunnelDeviceRemoteAddress)
{
    int sock;
    qInfo() << "Attempting to connect to Netlink";

    if(_sockFd != -1)
    {
        qInfo() << "Existing connection already exists, disconnecting first";
        shutdownConnection();
    }

    // Set SOCK_CLOEXEC to prevent socket being inherited by child processes (such as openvpn)
    sock = ::socket(PF_NETLINK, SOCK_DGRAM|SOCK_CLOEXEC, NETLINK_CONNECTOR);
    if(sock == -1)
    {
        showError("::socket");
        return;
    }

    sockaddr_nl address = {};

    address.nl_pid = getpid();
    address.nl_groups = CN_IDX_PROC;
    address.nl_family = AF_NETLINK;

    if(::bind(sock, reinterpret_cast<sockaddr*>(&address), sizeof(sockaddr_nl)) == -1)
    {
        showError("::bind");
        ::close(sock);
        return;
    }

    if(subscribeToProcEvents(sock, true) == -1)
    {
        qWarning() << "Could not subscribe to proc events";
        ::close(sock);
        return;
    }

    qInfo() << "Successfully connected to Netlink";

    // Save the socket FD to an ivar
    _sockFd = sock;
    setupFirewall();
    updateSplitTunnel(params, tunnelDeviceName, tunnelDeviceLocalAddress,
                      tunnelDeviceRemoteAddress, params.excludeApps,
                      params.vpnOnlyApps);
    setupReversePathFiltering();
    _readNotifier = new QSocketNotifier(sock, QSocketNotifier::Read);
    connect(_readNotifier, &QSocketNotifier::activated, this, &ProcTracker::readFromSocket);
}

void ProcTracker::setupReversePathFiltering()
{
    int exitCode;
    QString out, err;
    std::tie(exitCode, out, err) = ::shellExecute("sysctl -n 'net.ipv4.conf.all.rp_filter'");

    if(!exitCode)
    {
        if(out.toInt() != 2)
        {
            _previousRPFilter = out;
            qInfo() << "Storing old net.ipv4.conf.all.rp_filter value:" << out;
            qInfo() << "Setting rp_filter to loose";
            ::shellExecute("sysctl -w 'net.ipv4.conf.all.rp_filter=2'");
        }
        else
            qInfo() << "rp_filter already 2 (loose mode); nothing to do!";
    }
    else
    {
        qWarning() << "Unable to store old net.ipv4.conf.all.rp_filter value";
        _previousRPFilter = "";
        return;
    }
}

void ProcTracker::teardownReversePathFiltering()
{
    if(!_previousRPFilter.isEmpty())
    {
        qInfo() << "Restoring rp_filter to: " << _previousRPFilter;
        ::shellExecute(QStringLiteral("sysctl -w 'net.ipv4.conf.all.rp_filter=%1'").arg(_previousRPFilter));
    }
}

void ProcTracker::updateApps(QVector<QString> excludedApps, QVector<QString> vpnOnlyApps)
{
    qInfo() << "Inside updateApps." << "excludedApps:" << excludedApps << "vpnOnlyApps:" << vpnOnlyApps;
    // If we're not tracking excluded apps, remove everything
    if(!_previousNetScan.isValid())
        excludedApps = {};
    // Update excluded apps
    removeApps(excludedApps, _exclusionsMap);
    addApps(excludedApps, _exclusionsMap, Path::VpnExclusionsFile);

    // Update vpnOnly
    removeApps(vpnOnlyApps, _vpnOnlyMap);
    addApps(vpnOnlyApps, _vpnOnlyMap, Path::VpnOnlyFile);
}

void ProcTracker::removeAllApps()
{
    qInfo() << "Removing all apps from cgroups";
    removeApps({}, _exclusionsMap);
    removeApps({}, _vpnOnlyMap);

    _exclusionsMap.clear();
    _vpnOnlyMap.clear();
}

void ProcTracker::addApps(const QVector<QString> &apps, AppMap &appMap, QString cGroupPath)
{
    for(auto &app : apps)
    {
        appMap.insert(app, {});
        for(pid_t pid : ProcFs::pidsForPath(app))
        {
            // Both these calls are no-ops if the PID is already excluded
            addPidToCgroup(pid, cGroupPath);
            appMap[app].insert(pid);
        }
    }
}

void ProcTracker::removeApps(const QVector<QString> &keepApps, AppMap &appMap)
{
    for(const auto &app : appMap.keys())
    {
        if(!keepApps.contains(app))
        {
            for(pid_t pid : appMap[app])
            {
                removePidFromCgroup(pid, Path::ParentVpnExclusionsFile);
            }

            appMap.remove(app);
        }
    }
}

int ProcTracker::subscribeToProcEvents(int sock, bool enabled)
{
    NetlinkRequest message = {};

    message.subscription_type = enabled ? PROC_CN_MCAST_LISTEN : PROC_CN_MCAST_IGNORE;

    message.header.nlmsg_len = sizeof(message);
    message.header.nlmsg_pid = getpid();
    message.header.nlmsg_type = NLMSG_DONE;

    message.body.len = sizeof(proc_cn_mcast_op);
    message.body.id.val = CN_VAL_PROC;
    message.body.id.idx = CN_IDX_PROC;

    if(::send(sock, &message, sizeof(message), 0) == -1)
    {
        showError("::send");
        return -1;
    }

    return 0;
}

void ProcTracker::setupFirewall()
{
    // Setup the packet tagging rule (this rule is unaffected by network changes)
    // This rule also has callbacks that sets up the cgroup and the routing policy
    IpTablesFirewall::setAnchorEnabled(IpTablesFirewall::Both, QStringLiteral("100.tagPkts"), true, IpTablesFirewall::kMangleTable);

    // Enable the masquerading rule - this gets updated with interface changes via replaceAnchor()
    IpTablesFirewall::setAnchorEnabled(IpTablesFirewall::Both, QStringLiteral("100.transIp"), true, IpTablesFirewall::kNatTable);
}

void ProcTracker::teardownFirewall()
{
    // Remove the masquerading rule
    IpTablesFirewall::setAnchorEnabled(IpTablesFirewall::Both, QStringLiteral("100.transIp"), false, IpTablesFirewall::kNatTable);
    // Remove the cgroup marking rule
    IpTablesFirewall::setAnchorEnabled(IpTablesFirewall::Both, QStringLiteral("100.tagPkts"), false, IpTablesFirewall::kMangleTable);
}

void ProcTracker::addRoutingPolicyForSourceIp(QString ipAddress, QString routingTableName)
{
    if(!ipAddress.isEmpty())
        ::shellExecute(QStringLiteral("ip rule add from %1 lookup %3 pri 101")
            .arg(ipAddress, routingTableName));
}

void ProcTracker::removeRoutingPolicyForSourceIp(QString ipAddress, QString routingTableName)
{
    if(!ipAddress.isEmpty())
        ::shellExecute(QStringLiteral("ip rule del from %1 lookup %3 pri 101")
            .arg(ipAddress, routingTableName));
}

void ProcTracker::shutdownConnection()
{
    qInfo() << "Attempting to disconnect from Netlink";
    if(_readNotifier)
    {
        _readNotifier->setEnabled(false);
        delete _readNotifier;
    }

    if(_sockFd != -1)
    {
        // Unsubscribe from proc events
        subscribeToProcEvents(_sockFd, false);
        if(::close(_sockFd) != 0)
            showError("::close");
    }

    teardownFirewall();
    removeAllApps();
    removeRoutingPolicyForSourceIp(_previousNetScan.ipAddress(), IpTablesFirewall::kRtableName);
    removeRoutingPolicyForSourceIp(_previousTunnelDeviceLocalAddress, IpTablesFirewall::kVpnOnlyRtableName);
    teardownReversePathFiltering();

    // Clear out our network info
    _previousNetScan = {};
    _sockFd = -1;

    qInfo() << "Successfully disconnected from Netlink";
}

void ProcTracker::updateSplitTunnel(const FirewallParams &params, QString tunnelDeviceName,
                                    QString tunnelDeviceLocalAddress, QString tunnelDeviceRemoteAddress,
                                    QVector<QString> excludedApps, QVector<QString> vpnOnlyApps)
{
    // Update network first, then updateApps() can add/remove all excluded apps
    // when we gain/lose a valid network scan
    updateNetwork(params, tunnelDeviceName, tunnelDeviceLocalAddress, tunnelDeviceRemoteAddress);
    updateApps(excludedApps, vpnOnlyApps);
}

void ProcTracker::removeTerminatedApp(pid_t pid)
{
    // Remove from exclusions
    for(AppMap::iterator i = _exclusionsMap.begin(); i != _exclusionsMap.end(); ++i)
    {
        auto &set = i.value();
        set.remove(pid);
    }

    // Remove from vpnOnly
    for(AppMap::iterator i = _vpnOnlyMap.begin(); i != _vpnOnlyMap.end(); ++i)
    {
        auto &set = i.value();
        set.remove(pid);
    }
}

void ProcTracker::addLaunchedApp(pid_t pid)
{
    // Get the launch path associated with the PID
    QString appName = ProcFs::pathForPid(pid);

    // May be empty if the process was so short-lived it exited before we had a chance to read its name
    // In this case we just early-exit and ignore it
    if(appName.isEmpty())
        return;

    if(_exclusionsMap.contains(appName))
    {
        // Add it if we're currently tracking excluded apps.
        if(_previousNetScan.isValid())
        {
            _exclusionsMap[appName].insert(pid);
            qInfo() << "Adding" << pid << "to VPN exclusions for app:" << appName;

            // Add the PID to the cgroup so its network traffic goes out the
            // physical uplink
            addPidToCgroup(pid, Path::VpnExclusionsFile);
        }
    }
    else if(_vpnOnlyMap.contains(appName))
    {
        _vpnOnlyMap[appName].insert(pid);
        qInfo() << "Adding" << pid << "to VPN Only for app:" << appName;

        // Add the PID to the cgroup so its network traffic is forced out the
        // VPN
        addPidToCgroup(pid, Path::VpnOnlyFile);
    }
}

void ProcTracker::readFromSocket(int sock)
{
    NetlinkResponse message = {};

    ::recv(sock, &message, sizeof(message), 0);

    // shortcut
    const auto &eventData = message.event.event_data;
    pid_t pid;
    QString appName;

    switch(message.event.what)
    {
    case proc_event::PROC_EVENT_NONE:
        qInfo() << "Listening to process events";
        break;
    case proc_event::PROC_EVENT_EXEC:
        pid = eventData.exec.process_pid;
        addLaunchedApp(pid);

        break;
    case proc_event::PROC_EVENT_EXIT:
        pid = eventData.exit.process_pid;
        removeTerminatedApp(pid);

        break;
    default:
        // We're not interested in any other events
        break;
    }
}
