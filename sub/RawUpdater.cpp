#include <QInputDialog>
#include "RawUpdater.hpp"

#ifndef __MINGW32__

#include <yaml-cpp/yaml.h>

#endif

#include "db/Database.hpp"
#include "ui/mainwindow_message.h"
#include "qv2ray/utils/HTTPRequestHelper.hpp"
#include "db/filter/ProfileFilter.hpp"

namespace NekoRay::sub {
    void RawUpdater::update(const QString &str) {
        // Base64 encoded subscription
        if (auto str2 = DecodeB64IfValid(str);!str2.isEmpty()) {
            update(str2);
            return;
        }

        // Clash
        if (str.contains("proxies:")) {
            updateClash(str);
            return;
        }

        // Multi line
        if (str.count("\n") > 0) {
            auto list = str.split("\n");
            for (const auto &str2: list) {
                update(str2.trimmed());
            }
            return;
        }

        QSharedPointer<ProxyEntity> ent;

        // SOCKS
        if (str.startsWith("socks5://") || str.startsWith("socks4://") ||
            str.startsWith("socks4a://") || str.startsWith("socks://")) {
            ent = ProfileManager::NewProxyEntity("socks");
            auto ok = ent->SocksBean()->TryParseLink(str);
            if (!ok) ent = nullptr;
        }

        // ShadowSocks
        if (str.startsWith("ss://")) {
            ent = ProfileManager::NewProxyEntity("shadowsocks");
            auto ok = ent->ShadowSocksBean()->TryParseLink(str);
            if (!ok) ent = nullptr;
        }

        // ShadowSocksR (as ss stream + plugin)
        if (str.startsWith("ssr://")) {
            ent = ProfileManager::NewProxyEntity("shadowsocks");
            auto ok = ent->ShadowSocksBean()->TryParseSSR(str);
            if (!ok) ent = nullptr;
        }

        // VMess
        if (str.startsWith("vmess://")) {
            ent = ProfileManager::NewProxyEntity("vmess");
            auto ok = ent->VMessBean()->TryParseLink(str);
            if (!ok) ent = nullptr;
        }

        // Trojan
        if (str.startsWith("trojan://")) {
            ent = ProfileManager::NewProxyEntity("trojan");
            auto ok = ent->TrojanBean()->TryParseLink(str);
            if (!ok) ent = nullptr;
        }

        // End
        if (ent.get() == nullptr) return;
        profileManager->AddProfile(ent, update_sub_gid);
        dataStore->updated_count++;
    }

#ifndef __MINGW32__

    QString Node2QString(const YAML::Node &n, const QString &def = "") {
        try {
            return n.as<std::string>().c_str();
        } catch (const YAML::Exception &ex) {
            return def;
        }
    }

    int Node2Int(const YAML::Node &n, const int &def = 0) {
        try {
            return n.as<int>();
        } catch (const YAML::Exception &ex) {
            return def;
        }
    }

    bool Node2Bool(const YAML::Node &n, const bool &def = false) {
        try {
            return n.as<bool>();
        } catch (const YAML::Exception &ex) {
            return def;
        }
    }

#endif

    void RawUpdater::updateClash(const QString &str) {
#ifndef __MINGW32__
        try {
            auto proxies = YAML::Load(str.toStdString())["proxies"];
            for (auto proxy: proxies) {
                auto type = Node2QString(proxy["type"]);
                if (type == "ss" || type == "ssr") type = "shadowsocks";

                auto ent = ProfileManager::NewProxyEntity(type);
                ent->bean->name = Node2QString(proxy["name"]);
                ent->bean->serverAddress = Node2QString(proxy["server"]);
                ent->bean->serverPort = Node2Int(proxy["port"]);

                if (type == "shadowsocks") {
                    auto bean = ent->ShadowSocksBean();
                    bean->method = Node2QString(proxy["cipher"]).replace("dummy", "none");
                    bean->password = Node2QString(proxy["password"]);
                    auto plugin_n = proxy["plugin"];
                    auto pluginOpts_n = proxy["plugin-opts"];
                    if (plugin_n.IsDefined() && pluginOpts_n.IsDefined()) {
                        if (Node2QString(plugin_n) == "obfs") {
                            bean->plugin = "obfs-local;obfs=" + Node2QString(pluginOpts_n["mode"]) + ";obfs-host=" +
                                           Node2QString(pluginOpts_n["host"]);
                        }
                    }
                    auto protocol_n = proxy["protocol"];
                    if (protocol_n.IsDefined()) { // SSR
                        continue; // TODO SSR
                    }
                } else if (type == "trojan") {
                    auto bean = ent->TrojanBean();
                    bean->password = Node2QString(proxy["password"]);
                    bean->stream->sni = Node2QString(proxy["sni"]);
                    if (Node2Bool(proxy["skip-cert-verify"])) bean->stream->allow_insecure = true;
                } else if (type == "vmess") {
                    auto bean = ent->VMessBean();
                    bean->uuid = Node2QString(proxy["uuid"]);
                    bean->aid = Node2Int(proxy["alterId"]);
                    bean->security = Node2QString(proxy["cipher"]);
                    bean->stream->network = Node2QString(proxy["network"]);
                    bean->stream->sni = Node2QString(proxy["sni"]);
                    if (Node2Bool(proxy["tls"])) bean->stream->security = "tls";
                    if (Node2Bool(proxy["skip-cert-verify"])) bean->stream->allow_insecure = true;

                    auto ws = proxy["ws-opts"];
                    if (ws.IsMap()) {
                        auto headers = ws["headers"];
                        for (auto header: headers) {
                            if (Node2QString(header.first).toLower() == "host") {
                                bean->stream->host = Node2QString(header.second);
                            }
                        }
                        bean->stream->path = Node2QString(ws["path"]);
                        bean->stream->max_early_data = Node2Int(proxy["max-early-data"]);
                        bean->stream->early_data_header_name = Node2QString(ws["early-data-header-name"]);
                    }
                } else {
                    continue;
                }

                profileManager->AddProfile(ent, update_sub_gid);
                dataStore->updated_count++;
            }
        } catch (const YAML::Exception &ex) {
            runOnUiThread([=] {
                MessageBoxWarning("YAML Exception", ex.what());
            });
        }
#endif
    }

    // 各种奇妙UI
    // 不要刷新，下载导入完会自己刷新
    void RawUpdater::AsyncUpdate(const QString &str, int _update_sub_gid) {
        dataStore->updated_count = 0;
        this->update_sub_gid = _update_sub_gid;

        auto content = str.trimmed();
        bool asURL = update_sub_gid >= 0; // 订阅

        if (!asURL && (content.startsWith("http://") || content.startsWith("https://"))) {
            auto items = QStringList{QObject::tr("As Subscription"), QObject::tr("As link")};
            bool ok;
            auto a = QInputDialog::getItem(nullptr,
                                           QObject::tr("url detected"),
                                           QObject::tr("%1\nHow to update?").arg(content),
                                           items, 0, false, &ok);
            if (!ok) return;
            if (items.indexOf(a) == 0) asURL = true;
        }

        runOnNewThread([=] {
            auto content2 = content;
            auto group = profileManager->GetGroup(update_sub_gid);

            if (asURL) {
                showLog("URL=" + content2);
                content2 = NetworkRequestHelper::HttpGet(content2);
                showLog("Content=" + content2);
            }

            QList<QSharedPointer<ProxyEntity>> in; // 更新前
            QList<QSharedPointer<ProxyEntity>> out_all; // 更新前 + 更新后
            QList<QSharedPointer<ProxyEntity>> out; // 更新后
            QList<QSharedPointer<ProxyEntity>> only_in; // 只在更新前有的
            QList<QSharedPointer<ProxyEntity>> only_out; // 只在更新后有的
            QList<QSharedPointer<ProxyEntity>> update_del; // 更新前后都有的，删除更新后多余的

            if (group != nullptr) {// 订阅更新前
                in = group->Profiles();
            }

            // 解析并添加 profile
            update(content2);

            if (group != nullptr) {// 订阅更新完
                out_all = group->Profiles();

                ProfileFilter::OnlyInSrc_ByPointer(out_all, in, out);
                ProfileFilter::OnlyInSrc(in, out, only_in);
                ProfileFilter::OnlyInSrc(out, in, only_out);
                ProfileFilter::Common(in, out, update_del, false, true);
                update_del += only_in;

                for (const auto &ent: update_del) {
                    dataStore->updated_count--;
                    profileManager->DeleteProfile(ent->id);
                }

                qDebug() << "in" << in;
                qDebug() << "out_all" << out_all;
                qDebug() << "out" << out;
                qDebug() << "only_in" << only_in;
                qDebug() << "only_out" << only_out;
                qDebug() << "update_del" << update_del;

                QString notice_added;
                for (const auto &ent: only_out) {
                    notice_added += ent->bean->DisplayTypeAndName() + "\n";
                }
                QString notice_deleted;
                for (const auto &ent: only_in) {
                    notice_deleted += ent->bean->DisplayTypeAndName() + "\n";
                }
                runOnUiThread([=] {
                    MessageBoxWarning(QObject::tr("Change"),
                                      QObject::tr("Added %1 profiles:\n%2\nDeleted %3 Profiles:\n%4").
                                              arg(only_out.length()).arg(notice_added).
                                              arg(only_in.length()).arg(notice_deleted));
                    GetMainWindow()->dialog_message("SubUpdater", "finish-dingyue");
                });
            } else {
                runOnUiThread([=] {
                    GetMainWindow()->dialog_message("SubUpdater", "finish");
                });
            }
        });
    }
}