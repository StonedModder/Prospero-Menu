// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from PS Neighborhood platform plumbing; see vendor/runtime-provenance.json.
import { createServer as tcpServer } from './net.mjs';
import { createHttpServer } from './http-server.mjs';
export function createServer(options,handler) { return createHttpServer(tcpServer,options,handler); }
export function request() { throw Error('PS5 native package services are required'); }
export default { createServer,request };
