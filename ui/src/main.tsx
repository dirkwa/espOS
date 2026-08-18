// SPDX-License-Identifier: Apache-2.0
import { render } from "preact";
import { App } from "./app";
import { connectEvents } from "./api";
import "./style.css";

connectEvents();
render(<App />, document.getElementById("app")!);
