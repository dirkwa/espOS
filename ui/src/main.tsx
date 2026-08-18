// SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
import { render } from "preact";
import { App } from "./app";
import { connectEvents } from "./api";
import "./style.css";

connectEvents();
render(<App />, document.getElementById("app")!);
