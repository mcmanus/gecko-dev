/* -*- Mode: Java; c-basic-offset: 4; tab-width: 4; indent-tabs-mode: nil; -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.gecko;

import android.util.Log;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.ArrayList;

public class NativeQueue {
    private static final String LOGTAG = "GeckoNativeQueue";

    public interface State {
        boolean is(final State other);
        boolean isAtLeast(final State other);
    }

    public static class StateHolder {
        private volatile State mState;
        private final State mReadyState;

        public StateHolder(final State initial, final State ready) {
            this.mState = initial;
            this.mReadyState = ready;
        }

        public boolean isReady() {
            return getState().isAtLeast(mReadyState);
        }

        public State getReadyState() {
            return mReadyState;
        }

        public State getState() {
            return mState;
        }

        public boolean setState(final State newState) {
            return checkAndSetState(null, newState);
        }

        public boolean checkAndSetState(final State expectedState,
                                        final State newState) {
            synchronized (NativeQueue.sQueue) {
                if (expectedState != null && !mState.is(expectedState)) {
                    return false;
                }
                NativeQueue.flushQueuedLocked(newState);
                mState = newState;
                return true;
            }
        }
    }

    private static class QueuedCall {
        public Method method;
        public Object target;
        public Object[] args;
        public State state;

        public QueuedCall(final Method method, final Object target,
                          final Object[] args, final State state) {
            this.method = method;
            this.target = target;
            this.args = args;
            this.state = state;
        }
    }

    private static final int QUEUED_CALLS_COUNT = 16;
    /* package */ static final ArrayList<QueuedCall> sQueue =
        new ArrayList<>(QUEUED_CALLS_COUNT);

    // Invoke the given Method and handle checked Exceptions.
    private static void invokeMethod(final Method method, final Object obj,
                                     final Object[] args) {
        try {
            method.setAccessible(true);
            method.invoke(obj, args);
        } catch (final IllegalAccessException e) {
            throw new IllegalStateException("Unexpected exception", e);
        } catch (final InvocationTargetException e) {
            throw new UnsupportedOperationException("Cannot make call", e.getCause());
        }
    }

    // Queue a call to the given method.
    private static void queueNativeCallLocked(final StateHolder stateHolder,
                                              final Class<?> cls,
                                              final String methodName,
                                              final Object obj,
                                              final Object[] args,
                                              final State state) {
        final ArrayList<Class<?>> argTypes = new ArrayList<>(args.length);
        final ArrayList<Object> argValues = new ArrayList<>(args.length);

        for (int i = 0; i < args.length; i++) {
            if (args[i] instanceof Class) {
                argTypes.add((Class<?>) args[i]);
                argValues.add(args[++i]);
                continue;
            }
            Class<?> argType = args[i].getClass();
            if (argType == Boolean.class) argType = Boolean.TYPE;
            else if (argType == Byte.class) argType = Byte.TYPE;
            else if (argType == Character.class) argType = Character.TYPE;
            else if (argType == Double.class) argType = Double.TYPE;
            else if (argType == Float.class) argType = Float.TYPE;
            else if (argType == Integer.class) argType = Integer.TYPE;
            else if (argType == Long.class) argType = Long.TYPE;
            else if (argType == Short.class) argType = Short.TYPE;
            argTypes.add(argType);
            argValues.add(args[i]);
        }
        final Method method;
        try {
            method = cls.getDeclaredMethod(
                    methodName, argTypes.toArray(new Class<?>[argTypes.size()]));
        } catch (final NoSuchMethodException e) {
            throw new IllegalArgumentException("Cannot find method", e);
        }

        if (!Modifier.isNative(method.getModifiers())) {
            // As a precaution, we disallow queuing non-native methods. Queuing non-native
            // methods is dangerous because the method could end up being called on either
            // the original thread or the Gecko thread depending on timing. Native methods
            // usually handle this by posting an event to the Gecko thread automatically,
            // but there is no automatic mechanism for non-native methods.
            throw new UnsupportedOperationException("Not allowed to queue non-native methods");
        }

        if (stateHolder.getState().isAtLeast(state)) {
            invokeMethod(method, obj, argValues.toArray());
            return;
        }

        sQueue.add(new QueuedCall(
                method, obj, argValues.toArray(), state));
    }

    /**
     * Queue a call to the given instance method if the given current state does
     * not satisfy the given state.
     *
     * @param stateHolder The state holder used to query the current state.
     * @param state The state in which the native call could be executed.
     * @param obj Object that declares the instance method.
     * @param methodName Name of the instance method.
     * @param args Args to call the instance method with; to specify a parameter
     *             type, pass in a Class instance first, followed by the value.
     */
    public static void queueUntil(final StateHolder stateHolder,
                                  final State state,
                                  final Object obj,
                                  final String methodName,
                                  final Object... args) {
        synchronized (sQueue) {
            queueNativeCallLocked(stateHolder, obj.getClass(), methodName, obj,
                                  args, state);
        }
    }

    /**
     * Queue a call to the given static method if the given current state does
     * not satisfy the given state.
     *
     * @param stateHolder The state holder used to query the current state.
     * @param state The state in which the native call could be executed.
     * @param cls Class that declares the static method.
     * @param methodName Name of the instance method.
     * @param args Args to call the instance method with; to specify a parameter
     *             type, pass in a Class instance first, followed by the value.
     */
    public static void queueUntil(final StateHolder stateHolder,
                                  final State state,
                                  final Class<?> cls,
                                  final String methodName,
                                  final Object... args) {
        synchronized (sQueue) {
            queueNativeCallLocked(stateHolder, cls, methodName, null, args, state);
        }
    }

    // Run all queued methods
    private static void flushQueuedLocked(final State state) {
        int lastSkipped = -1;
        for (int i = 0; i < sQueue.size(); i++) {
            final QueuedCall call = sQueue.get(i);
            if (call == null) {
                // We already handled the call.
                continue;
            }
            if (!state.isAtLeast(call.state)) {
                // The call is not ready yet; skip it.
                lastSkipped = i;
                continue;
            }
            // Mark as handled.
            sQueue.set(i, null);

            invokeMethod(call.method, call.target, call.args);
        }
        if (lastSkipped < 0) {
            // We're done here; release the memory
            sQueue.clear();
            sQueue.trimToSize();
        } else if (lastSkipped < sQueue.size() - 1) {
            // We skipped some; free up null entries at the end,
            // but keep all the previous entries for later.
            sQueue.subList(lastSkipped + 1, sQueue.size()).clear();
        }
    }
}
