// SPDX-License-Identifier: Apache-2.0
import { get, type SystemInfo } from "../api";
import { useAsync, Row } from "../app";

export function OtaPage() {
  const info = useAsync(() => get<SystemInfo>("/system/info"));
  return (
    <>
      <h1>OTA</h1>
      <section class="card">
        <h2>Firmware</h2>
        {info.data && <><Row k="Running">{info.data.app} {info.data.version}</Row><Row k="ESP-IDF">{info.data.idf_version}</Row></>}
        <p class="muted">Over-the-air update (from a URL or a version manifest, signed, with automatic rollback) arrives with milestone M6. Until then flash over USB: <code>idf.py -p PORT flash</code>.</p>
      </section>
    </>
  );
}
