/* @refresh reload */
import { render } from "solid-js/web";
import "@fontsource/roboto/400.css";
import "@fontsource/roboto/500.css";
import "@fontsource/roboto/700.css";
import "./index.css";
import App from "./App.tsx";

render(() => <App />, document.getElementById("root")!);
